#include "campodata/FirmwareUpdater.h"

#include "campodata/detail/BuildTargets.h"

#if FWUP_TARGET_ARDUINO

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>

#include "core/Backoff.h"
#include "core/Log.h"
#include "core/DeviceStore.h"
#include "core/HardwareModel.h"
#include "core/OrderCodec.h"
#include "core/PingBuilder.h"
#include "core/ProvisioningCodec.h"
#include "core/RemoteConfigCodec.h"
#include "core/TopicResolver.h"
#include "core/Url.h"
#include "platform/esp32/ArduinoClock.h"
#include "platform/esp32/Esp32Hardware.h"
#include "platform/esp32/Esp32HttpClient.h"
#include "platform/esp32/Esp32Nvs.h"
#include "platform/esp32/Esp32OtaSink.h"
#include "platform/esp32/EspMqttClient.h"
#include "platform/esp32/RollbackGuard.h"

// Keeps the OTA rollback window open past initArduino().
//
// Arduino ships this weak and returning false, and reacts by marking any
// PENDING_VERIFY image valid before setup() runs - closing the window before
// the application can decide anything. Overriding it moves that decision here,
// where it is gated on the confirm response.
//
// It lives in this translation unit, alongside FirmwareUpdater::begin(), so the
// linker always pulls the object in. Alone in its own file the archive member
// might never be extracted and the weak default would silently win.
extern "C" bool verifyRollbackLater() {
    return true;
}

namespace campodata {
namespace {

constexpr uint32_t kProvisionRetryMs   = 15000;
constexpr uint32_t kMqttRetryMs        = 5000;
constexpr uint32_t kConfirmRetryMs     = 10000;
constexpr size_t   kJsonScratch        = 1024;
constexpr size_t   kPingScratch        = 768;
constexpr uint32_t kOtaChunkBuffer     = 1024;

bool linkUp() {
    return WiFi.status() == WL_CONNECTED;
}

// A pilha so consegue resolver nome com servidor DNS configurado. Checar antes
// evita abrir uma sessao TLS - que aloca dezenas de KB de heap - so para
// descobrir que o nome nao vira IP.
bool dnsReady() {
    return WiFi.status() == WL_CONNECTED && WiFi.dnsIP() != IPAddress(0, 0, 0, 0);
}

// Nome que nao resolve e quase sempre rede sem DNS, nao problema do servidor.
// Um DNS em 0.0.0.0 significa que o DHCP nao entregou nenhum, e ai nenhuma
// chamada por nome vai funcionar - nem NTP, nem HTTPS.
void logNetwork() {
    if (WiFi.status() != WL_CONNECTED) {
        FWUP_LOGW("net", "wifi desconectado");
        return;
    }

    const IPAddress dns = WiFi.dnsIP();
    FWUP_LOGI("net", "ip=%s gw=%s dns=%s rssi=%d",
              WiFi.localIP().toString().c_str(),
              WiFi.gatewayIP().toString().c_str(),
              dns.toString().c_str(),
              (int)WiFi.RSSI());

    if (dns == IPAddress(0, 0, 0, 0)) {
        FWUP_LOGE("net", "sem servidor DNS: o DHCP nao entregou um. "
                         "Nenhuma chamada por nome vai resolver.");
    }
}

}  // namespace

struct FirmwareUpdater::Impl {
    Config          cfg;
    Esp32Nvs        nvs;
    Esp32HttpClient http;
    EspMqttClient mqtt_esp;
    IMqttClient*  mqtt = &mqtt_esp;
    Esp32OtaSink    ota;
    ArduinoClock    clock;
    TopicResolver   topics;
    PingBuilder     ping_builder;
    HardwareInfo    hw;
    Capabilities    caps;

    DeviceStore* store = nullptr;

    DeviceState state        = DeviceState::Boot;
    OtaState    ota_state    = OtaState::Idle;
    AbortReason abort_reason = AbortReason::None;

    char board_id[16]              = {};
    char hardware_model[kMaxModelLen] = {};
    char device_id[kMaxIdLen]      = {};
    char client_id[kMaxIdLen]      = {};

    // Pending order being evaluated or downloaded.
    UpdateOrder order;
    bool        has_order      = false;
    uint32_t    button_deadline_ms = 0;
    uint32_t    dl_started_ms  = 0;
    uint32_t    last_progress_ms = 0;
    uint8_t     dl_attempts    = 0;

    RemoteConfig remote_cfg;

    // Negacao e falha de rede pedem ritmos diferentes: um 403 espera alguem no
    // painel, uma rede instavel volta sozinha em segundos.
    Backoff  provision_backoff;
    Backoff  transport_backoff;
    bool     warned_no_dns = false;
    Backoff  mqtt_backoff;
    Backoff  confirm_backoff;
    uint32_t last_ping_ms  = 0;
    uint32_t last_mqtt_try = 0;
    bool     mqtt_started  = false;
    bool     was_connected = false;

    MqttStatus mqtt_status = MqttStatus::Ok;

    // Subscriptions registered by the project, reapplied on every reconnect.
    static constexpr size_t kMaxSubs = 8;
    char    subs[kMaxSubs][kMaxTopicLen] = {};
    uint8_t subs_qos[kMaxSubs]           = {};
    size_t  subs_count                   = 0;

    PingExtendCb   cb_ping        = nullptr;  void* cb_ping_ctx   = nullptr;
    RemoteConfigCb cb_config      = nullptr;  void* cb_config_ctx = nullptr;
    OtaStateCb     cb_ota         = nullptr;  void* cb_ota_ctx    = nullptr;
    OrderVetoCb    cb_veto        = nullptr;  void* cb_veto_ctx   = nullptr;
    MqttMessageCb  cb_message     = nullptr;  void* cb_msg_ctx    = nullptr;
    MqttEventCb    cb_connect     = nullptr;  void* cb_conn_ctx   = nullptr;
    MqttEventCb    cb_disconnect  = nullptr;  void* cb_disc_ctx   = nullptr;

    char scratch[kJsonScratch] = {};

    void setOtaState(OtaState next, AbortReason reason) {
        if (next == ota_state && reason == abort_reason) return;
        const OtaState from = ota_state;
        ota_state    = next;
        abort_reason = reason;
        store->setOtaState(next, reason);
        if (cb_ota != nullptr) cb_ota(from, next, cb_ota_ctx);
    }
};

// Direct-dispatch trampoline: only application topics reach it, because the
// library reserves its own three.
static void directMessage(const char* topic, const uint8_t* payload, size_t len,
                          void* ctx) {
    auto* impl = static_cast<FirmwareUpdater::Impl*>(ctx);
    if (impl != nullptr && impl->cb_message != nullptr) {
        impl->cb_message(topic, payload, len, impl->cb_msg_ctx);
    }
}

FirmwareUpdater::FirmwareUpdater() : _impl(new Impl()) {}

FirmwareUpdater::~FirmwareUpdater() {
    if (_impl != nullptr) {
        delete _impl->store;
        delete _impl;
    }
}

ConfigError FirmwareUpdater::begin(const Config& cfg) {
    Impl& d = *_impl;
    d.cfg = cfg;

    log::configure(d.cfg.log_sink, d.cfg.log_ctx, d.cfg.log_level);

    const ConfigError err = d.cfg.validate();
    if (err != ConfigError::Ok) {
        FWUP_LOGE("fwup", "config invalida: %s", toString(err));
        return err;
    }

    FWUP_LOGI("fwup", "iniciando  fw=%s  repo=%s", d.cfg.firmware_version, d.cfg.repo);

    if (!d.nvs.begin()) {
        FWUP_LOGE("nvs", "falhou ao abrir: seguindo sem persistencia");
        return ConfigError::Ok;
    }
    d.store = new DeviceStore(d.nvs);
    FWUP_LOGI("nvs", "aberta  criptografada=%s",
              d.store->encrypted() ? "sim" : "NAO");

    platform::probeHardware(d.hw);
    hardware::classify(d.hw, d.hardware_model, sizeof(d.hardware_model));
    hardware::boardId(d.hw.mac, d.board_id, sizeof(d.board_id));
    if (d.cfg.hardware_model_override != nullptr) {
        snprintf(d.hardware_model, sizeof(d.hardware_model), "%s",
                 d.cfg.hardware_model_override);
    }

    d.caps.ota_capable        = d.hw.ota_capable;
    d.caps.rollback_supported = rollback::supported();
    d.caps.sha256_supported   = true;
    // The deployed download endpoint returns the whole image, so resume is not
    // available. Reported rather than assumed.
    d.caps.range_supported      = false;
    d.caps.mqtt_max_publish_qos = d.mqtt->maxPublishQos();
    d.caps.tier                 = 1;

    FWUP_LOGI("board", "%s  board_id=%s", d.hardware_model, d.board_id);
    if (d.hw.ota_capable) {
        FWUP_LOGI("board", "slot OTA %u bytes em 0x%06X",
                  d.hw.ota.size_bytes, d.hw.ota.offset);
    } else {
        FWUP_LOGE("board", "SEM particao OTA: atualizacao impossivel nesta tabela");
    }

    d.http.configure(20000, d.cfg.endpoints.insecure_tls, d.cfg.endpoints.ca_pem);
    d.ping_builder.setExtender(d.cb_ping, d.cb_ping_ctx);

    // Restore what survived the reboot.
    d.ota_state    = d.store->otaState();
    d.abort_reason = d.store->abortReason();

    const bool pending = d.store->hasPending();

    // If a freshly applied image is already valid, something confirmed it
    // before us - the weak Arduino default. Record it: the failure is
    // otherwise completely silent and only shows up as a rollback that never
    // happens, months later.
    const auto guard = rollback::check(pending);
    if (guard == rollback::GuardState::Bypassed) {
        d.store->setRollbackGuardBypassed(true);
        FWUP_LOGE("rollback", "JANELA FECHADA ANTES DO SETUP: o simbolo forte "
                              "verifyRollbackLater() nao foi linkado");
    } else {
        FWUP_LOGI("rollback", "suportado=%s estado=%s",
                  rollback::supported() ? "sim" : "nao", rollback::toString(guard));
    }

    if (pending && d.ota.pendingVerify()) {
        d.state     = DeviceState::Confirming;
        d.ota_state = OtaState::Confirming;
    } else if (pending) {
        // Pending record with no pending image: the update never applied.
        d.store->clearPending();
        d.state = DeviceState::Operation;
    } else {
        d.state = DeviceState::Operation;
    }

    if (d.store->deviceId(d.device_id, sizeof(d.device_id))) {
        snprintf(d.client_id, sizeof(d.client_id), "%s", d.device_id);
        d.topics.build(d.cfg.topics, d.device_id);

        Provisioning stored;
        if (d.store->topics(stored.topic_ping, sizeof(stored.topic_ping),
                            stored.topic_update, sizeof(stored.topic_update),
                            stored.topic_config, sizeof(stored.topic_config))) {
            d.topics.applyProvisioned(stored);
        }
    } else {
        snprintf(d.client_id, sizeof(d.client_id), "%s", d.board_id);
    }

    d.provision_backoff.configure(backoff::kProvisionDenied,
                                  backoff::kProvisionDeniedCount);
    d.transport_backoff.configure(backoff::kNetwork, backoff::kNetworkCount);
    d.mqtt_backoff.configure(backoff::kMqtt, backoff::kMqttCount);
    d.confirm_backoff.configure(backoff::kConfirm, backoff::kConfirmCount);

    if (d.store->isProvisioned()) {
        FWUP_LOGI("fwup", "provisionado  device_id=%s", d.device_id);

        // O que veio do provisionamento e vive na NVS. Sem isso no boot, uma
        // credencial errada so apareceria como "nao conecta", sem dizer o que
        // o dispositivo esta de fato tentando usar. A senha nao entra no log.
        char host[64] = {}, user[48] = {};
        uint16_t port = 0;
        d.store->mqttHost(host, sizeof(host));
        d.store->mqttPort(port);
        d.store->mqttUser(user, sizeof(user));
        FWUP_LOGI("mqtt", "broker %s:%u  usuario=%s", host, port, user);

        char api[128] = {};
        if (d.store->apiBaseUrl(api, sizeof(api))) {
            FWUP_LOGI("fwup", "api=%s", api);
        }

        FWUP_LOGI("mqtt", "topicos  ping=%s  update=%s",
                  d.topics.ping(), d.topics.update());
    } else {
        FWUP_LOGI("fwup", "NAO provisionado: fara POST de provisionamento ao subir a rede");
    }

    d.clock.begin();
    return ConfigError::Ok;
}

// ---------------------------------------------------------------- helpers --

// api_base_url comes from provisioning; before that, the bootstrap URL is the
// only address the device has.
static bool apiBase(FirmwareUpdater::Impl& d, char* out, size_t cap) {
    if (d.store != nullptr && d.store->apiBaseUrl(out, cap)) return true;

    const char* fallback = (d.cfg.link_mode == LinkMode::Gprs)
                               ? d.cfg.endpoints.api_base_url_gprs
                               : d.cfg.endpoints.api_base_url;
    return d.store != nullptr && d.store->bootstrapUrl(out, cap, fallback);
}

static void provisionStep(FirmwareUpdater::Impl& d, uint32_t now) {
    if (!linkUp()) return;

    // Sem DNS a requisicao nao tem como chegar a lugar nenhum, entao nem sai.
    // O aviso sai uma vez por queda, e nao a cada volta do loop.
    if (!dnsReady()) {
        if (!d.warned_no_dns) {
            d.warned_no_dns = true;
            FWUP_LOGE("prov", "adiado: rede sem DNS, o nome do servidor nao "
                              "tem como ser resolvido");
            logNetwork();
        }
        return;
    }
    if (d.warned_no_dns) {
        d.warned_no_dns = false;
        FWUP_LOGI("prov", "DNS disponivel, retomando");
    }

    if (!d.provision_backoff.ready(now) || !d.transport_backoff.ready(now)) return;

    char base[128] = {};
    if (!apiBase(d, base, sizeof(base))) return;

    char url[kMaxUrlLen] = {};
    if (!url::join(base, d.cfg.endpoints.path_provisioning, url, sizeof(url))) return;

    ProvisionRequest req;
    req.board_id         = d.board_id;
    req.hardware_model   = d.hardware_model;
    req.firmware_version = d.cfg.firmware_version;
    req.repo             = d.cfg.repo;
    // Selects the broker profile server-side. Omitting it hands a cellular
    // device TLS credentials it cannot use.
    req.rede = (d.cfg.link_mode == LinkMode::Gprs) ? LinkType::Gprs : LinkType::Wifi;

    char body[256] = {};
    if (provisioning::buildRequest(req, body, sizeof(body)) != CodecError::Ok) return;

    d.state = DeviceState::Provisioning;
    FWUP_LOGI("prov", "POST %s", url);
    FWUP_LOGD("prov", "corpo: %s", body);

    HttpResponse res;
    const HttpError herr = d.http.postJson(url, body, d.scratch, kJsonScratch, res);
    if (herr != HttpError::Ok) {
        // Rede, e nao recusa: volta rapido em vez de esperar o backoff longo.
        d.transport_backoff.fail(now);
        FWUP_LOGE("prov", "nao alcancou o servidor (%s), nova tentativa em %u ms",
                  toString(herr), d.transport_backoff.currentDelayMs());
        logNetwork();
        return;
    }
    d.transport_backoff.reset();

    const ProvisionOutcome outcome = provisioning::fromHttpStatus(res.status);
    if (outcome != ProvisionOutcome::Granted) {
        // 400/403/404 need a human in the panel; retrying faster changes nothing.
        d.provision_backoff.fail(now);
        FWUP_LOGE("prov", "HTTP %d -> %s%s", res.status,
                  provisioning::toString(outcome),
                  provisioning::isTransient(outcome)
                      ? "" : " (precisa de acao no painel)");
        FWUP_LOGD("prov", "resposta: %s", d.scratch);
        return;
    }

    Provisioning p;
    const CodecError cerr = provisioning::parseResponse(d.scratch, p);
    if (cerr != CodecError::Ok) {
        FWUP_LOGE("prov", "resposta 200 invalida: %s", provisioning::toString(cerr));
        FWUP_LOGD("prov", "corpo: %s", d.scratch);
        d.provision_backoff.fail(now);
        return;
    }

    // Writes credentials and erases the bootstrap URL in one step: a
    // provisioned unit no longer carries the endpoint address.
    if (!d.store->saveProvisioning(p)) {
        FWUP_LOGE("prov", "falhou ao gravar na NVS: nao marca como provisionado");
        d.provision_backoff.fail(now);
        return;
    }

    snprintf(d.device_id, sizeof(d.device_id), "%s", p.device_id);
    snprintf(d.client_id, sizeof(d.client_id), "%s", p.device_id);
    d.topics.build(d.cfg.topics, d.device_id);
    d.topics.applyProvisioned(p);

    d.provision_backoff.reset();
    d.state = DeviceState::Operation;

    FWUP_LOGI("prov", "OK  device_id=%s  broker=%s:%u tls=%s",
              p.device_id, p.mqtt_host, p.mqtt_port, p.mqtt_tls ? "sim" : "nao");
    FWUP_LOGI("prov", "URL de bootstrap apagada da NVS");
}

static void applySubscriptions(FirmwareUpdater::Impl& d) {
    // Uma assinatura negada nao gera erro no broker: o dispositivo fica online
    // e simplesmente nunca recebe ordem. Registrar cada uma e o unico jeito de
    // enxergar isso.
    FWUP_LOGI("mqtt", "assinando %s (qos 1)", d.topics.update());
    d.mqtt->subscribe(d.topics.update(), 1);
    FWUP_LOGI("mqtt", "assinando %s (qos 1)", d.topics.config());
    d.mqtt->subscribe(d.topics.config(), 1);

    for (size_t i = 0; i < d.subs_count; ++i) {
        FWUP_LOGI("mqtt", "assinando %s (qos %u) [projeto]", d.subs[i], d.subs_qos[i]);
        d.mqtt->subscribe(d.subs[i], d.subs_qos[i]);
    }
}

static void mqttStep(FirmwareUpdater::Impl& d, uint32_t now) {
    if (!d.store->isProvisioned() || !d.topics.ready()) return;

    const bool up = d.mqtt->connected();

    if (up && !d.was_connected) {
        d.was_connected = true;
        FWUP_LOGI("mqtt", "conectado");
        applySubscriptions(d);
        d.mqtt_backoff.reset();
        if (d.cb_connect != nullptr) d.cb_connect(d.cb_conn_ctx);
    } else if (!up && d.was_connected) {
        d.was_connected = false;
        FWUP_LOGW("mqtt", "desconectado");
        if (d.cb_disconnect != nullptr) d.cb_disconnect(d.cb_disc_ctx);
    }

    if (up || !linkUp()) return;

    // O broker tambem e alcancado por nome: sem DNS a sessao nao teria como
    // subir, e esp-mqtt ficaria reciclando socket sozinho na propria task.
    if (!dnsReady()) return;

    if (d.mqtt_started || !d.mqtt_backoff.ready(now)) return;

    char host[64] = {}, user[48] = {}, pass[64] = {};
    uint16_t port = 0;
    if (!d.store->mqttHost(host, sizeof(host)) || !d.store->mqttPort(port) ||
        !d.store->mqttUser(user, sizeof(user)) || !d.store->mqttPass(pass, sizeof(pass))) {
        return;
    }

    MqttSessionConfig s;
    s.host = host;
    s.port = port;
    s.tls  = (port == 8883);

    if (d.cfg.mqtt.tls_mode == TlsMode::ForcePlain && s.tls) {
        FWUP_LOGW("mqtt", "TLS desativado por configuracao: %u -> %u em texto claro",
                  port, d.cfg.mqtt.plain_port);
        s.tls  = false;
        s.port = d.cfg.mqtt.plain_port;
    } else if (d.cfg.mqtt.tls_mode == TlsMode::ForceTls) {
        s.tls = true;
    }

    s.verify_ca = d.cfg.mqtt.verify_ca;
    s.ca_pem    = d.cfg.mqtt.ca_pem;
    s.username  = user;
    s.password  = pass;
    s.client_id = d.client_id;

    // Spec section 5: the will announces the drop on the ping topic, which the
    // same table marks as not retained.
    s.will_topic   = d.topics.ping();
    s.will_payload = PingBuilder::willPayload();
    s.will_qos     = PingBuilder::kWillQos;
    s.will_retain  = PingBuilder::kWillRetain;

    s.keepalive_s   = d.remote_cfg.mqtt_keepalive_s;
    s.buffer_bytes  = d.cfg.mqtt.buffer_bytes;
    s.clean_session = d.cfg.mqtt.clean_session;

    // The library's own topics always go through the queue, so the OTA state
    // machine never runs on the MQTT task whatever the project chose.
    d.mqtt->reserveTopic(d.topics.update());
    d.mqtt->reserveTopic(d.topics.config());
    if (d.cfg.mqtt.dispatch == DispatchMode::Async) {
        d.mqtt->setDirectCallback(directMessage, &d);
    }

    FWUP_LOGI("mqtt", "conectando %s:%u tls=%s usuario=%s",
              host, port, s.tls ? "sim" : "nao", user);

    // Vale dizer em voz alta: cifrado nao e o mesmo que autenticado. Sem
    // verificar o certificado, o trafego viaja protegido de quem so escuta,
    // mas nada garante que do outro lado esta o broker certo.
    // O esp-tls do Arduino recusa subir sem nenhuma opcao de verificacao: sem
    // CA, sem bundle e sem PSK ele devolve ESP_ERR_MBEDTLS_SSL_SETUP_FAILED.
    // Pular a verificacao nao e alternativa porque CONFIG_ESP_TLS_INSECURE vem
    // desligado no framework.
    if (s.tls && !s.verify_ca && s.ca_pem == nullptr) {
        FWUP_LOGW("mqtt", "TLS sem validar certificado: cifrado, porem sem "
                          "autenticar o servidor");
    }

    if (d.mqtt->begin(s)) {
        d.mqtt_started = true;
    } else {
        d.mqtt_backoff.fail(now);
        FWUP_LOGE("mqtt", "begin falhou, nova tentativa em %u ms",
                  d.mqtt_backoff.currentDelayMs());
    }
}

static void handleOrder(FirmwareUpdater::Impl& d, const char* payload, size_t len);
static void handleConfig(FirmwareUpdater::Impl& d, const char* payload, size_t len);

static void ingestStep(FirmwareUpdater::Impl& d) {
    char    topic[kMaxTopicLen] = {};
    uint8_t buf[kOtaChunkBuffer];
    size_t  len = 0;

    while (d.mqtt->poll(topic, sizeof(topic), buf, sizeof(buf) - 1, len)) {
        buf[len] = '\0';
        const char* text = reinterpret_cast<const char*>(buf);

        if (strcmp(topic, d.topics.update()) == 0) {
            handleOrder(d, text, len);
        } else if (strcmp(topic, d.topics.config()) == 0) {
            handleConfig(d, text, len);
        } else if (d.cb_message != nullptr) {
            d.cb_message(topic, buf, len, d.cb_msg_ctx);
        }
    }
}

static void handleConfig(FirmwareUpdater::Impl& d, const char* payload, size_t) {
    RemoteConfig incoming = d.remote_cfg;
    if (remoteconfig::parse(payload, incoming) != CodecError::Ok) return;

    // Retained topic: this arrives again on every reconnect, so only a strictly
    // newer version is applied.
    if (!d.store->applyConfigVersion(incoming.config_version)) return;

    d.remote_cfg = incoming;
    FWUP_LOGI("cfg", "versao %u aplicada  ping=%us  janela_botao=%us  ota_gprs=%s",
              incoming.config_version, incoming.ping_interval_s,
              incoming.ota_button_window_s,
              incoming.allow_ota_on_gprs ? "sim" : "nao");
    if (d.cb_config != nullptr) d.cb_config(d.remote_cfg, d.cb_config_ctx);
}

static void handleOrder(FirmwareUpdater::Impl& d, const char* payload, size_t) {
    UpdateOrder incoming;
    if (order::parse(payload, incoming) != CodecError::Ok) return;

    char current_version[kMaxVersionLen] = {};
    char current_repo[kMaxRepoLen]       = {};
    char last_id[kMaxIdLen]              = {};
    d.store->firmwareVersion(current_version, sizeof(current_version));
    d.store->firmwareRepo(current_repo, sizeof(current_repo));
    d.store->lastUpdateId(last_id, sizeof(last_id));

    if (current_version[0] == '\0') {
        snprintf(current_version, sizeof(current_version), "%s", d.cfg.firmware_version);
    }
    if (current_repo[0] == '\0') {
        snprintf(current_repo, sizeof(current_repo), "%s", d.cfg.repo);
    }

    const OrderRejection why = order::validate(incoming, current_version, current_repo,
                                               d.hw.ota.size_bytes, last_id,
                                               d.cfg.ota.allow_downgrade);
    FWUP_LOGI("ota", "ordem %s  versao %s -> %s  %u bytes",
              incoming.update_id, current_version, incoming.target_version,
              incoming.size_bytes);

    if (why == OrderRejection::Duplicate) {
        FWUP_LOGD("ota", "ja tratada, ignorando (topico retido reentrega)");
        return;
    }

    if (why != OrderRejection::None) {
        const AbortReason reason =
            (why == OrderRejection::TooLarge)   ? AbortReason::SizeMismatch :
            (why == OrderRejection::Downgrade)  ? AbortReason::Downgrade :
            (why == OrderRejection::WrongRepo)  ? AbortReason::ServerReject :
            (why == OrderRejection::NoSha256)   ? AbortReason::Checksum :
                                                  AbortReason::None;
        if (why == OrderRejection::SameVersion) {
            FWUP_LOGD("ota", "ja esta nesta versao");
        } else {
            FWUP_LOGE("ota", "ordem recusada: %s", order::toString(why));
            d.setOtaState(OtaState::Failed, reason);
        }
        return;
    }

    if (d.cb_veto != nullptr && !d.cb_veto(incoming, d.cb_veto_ctx)) {
        d.setOtaState(OtaState::Failed, AbortReason::AppVeto);
        return;
    }

    d.order     = incoming;
    d.has_order = true;
    d.state     = DeviceState::EvaluatingOrder;
}

// ------------------------------------------------------------------- ota ---

static bool otaLinkReady(FirmwareUpdater::Impl& d) {
    if (d.cfg.ota.link_policy == OtaLinkPolicy::WifiOnly) return linkUp();
    return linkUp() || d.cfg.ota.allow_ota_on_gprs;
}

static bool buttonPressed(FirmwareUpdater::Impl& d) {
    if (d.cfg.button.button_pin < 0) return false;
    const int level = digitalRead(d.cfg.button.button_pin);
    return d.cfg.button.button_active_low ? (level == LOW) : (level == HIGH);
}

static void failOta(FirmwareUpdater::Impl& d, AbortReason reason) {
    FWUP_LOGE("ota", "abortado: %s (escritos %u bytes)",
              toString(reason), d.ota.written());
    d.ota.abort();
    d.http.endDownload();
    d.has_order = false;
    d.state     = DeviceState::Operation;
    d.setOtaState(OtaState::Failed, reason);
}

static void startDownload(FirmwareUpdater::Impl& d, uint32_t now) {
    // The server builds every download URL from one configured base, so a
    // link that cannot do TLS may be handed an https:// address.
    if (!url::fetchable(d.order.url, d.http.supportsTls())) {
        failOta(d, AbortReason::NoWifi);
        return;
    }

    if (d.ota.begin(d.order.size_bytes) != OtaSinkError::Ok) {
        failOta(d, AbortReason::Flash);
        return;
    }

    if (d.http.beginDownload(d.order.url, 0) != HttpError::Ok) {
        failOta(d, AbortReason::Network);
        return;
    }

    FWUP_LOGI("ota", "baixando de %s", d.order.url);
    d.dl_started_ms    = now;
    d.last_progress_ms = now;
    d.state            = DeviceState::Downloading;
    d.setOtaState(OtaState::Downloading, AbortReason::None);
}

static void pumpDownload(FirmwareUpdater::Impl& d, uint32_t now) {
    // Time-based, not byte-based: erasing a 4 KB sector costs 25-45 ms, so a
    // byte budget has an unbounded worst case against a 5 s watchdog.
    const uint32_t deadline = now + d.cfg.ota.budget_ms;
    uint8_t  buf[kOtaChunkBuffer];
    uint32_t moved = 0;

    while (millis() < deadline && moved < d.cfg.ota.chunk_bytes) {
        size_t got = 0;
        const HttpError r = d.http.readChunk(buf, sizeof(buf), got);

        if (r == HttpError::Eof) {
            FWUP_LOGI("ota", "download completo: %u bytes", d.ota.written());
            d.http.endDownload();
            d.state = DeviceState::Verifying;
            d.setOtaState(OtaState::Verifying, AbortReason::None);
            return;
        }
        if (r == HttpError::WouldBlock) break;
        if (r != HttpError::Ok) {
            failOta(d, AbortReason::Network);
            return;
        }

        if (d.ota.write(buf, got) != OtaSinkError::Ok) {
            failOta(d, AbortReason::Flash);
            return;
        }
        moved += got;
        d.last_progress_ms = millis();
    }

    if ((millis() - d.last_progress_ms) > d.cfg.ota.stall_timeout_ms) {
        failOta(d, AbortReason::Stall);
    }
}

static void verifyAndApply(FirmwareUpdater::Impl& d, uint32_t now) {
    // finish() checks the digest and only then makes the image bootable, so an
    // unverified image can never be booted by mistake.
    const OtaSinkError r = d.ota.finish(d.order.has_sha256 ? d.order.sha256 : nullptr);
    if (r != OtaSinkError::Ok) {
        FWUP_LOGE("ota", "verificacao falhou: %s", toString(r));
        failOta(d, r == OtaSinkError::ChecksumMismatch ? AbortReason::Checksum
                                                       : AbortReason::Flash);
        return;
    }

    DeviceStore::Pending p;
    snprintf(p.update_id, sizeof(p.update_id), "%s", d.order.update_id);
    snprintf(p.version, sizeof(p.version), "%s", d.order.target_version);
    snprintf(p.repo, sizeof(p.repo), "%s", d.order.repo);

    char previous[kMaxVersionLen] = {};
    if (!d.store->firmwareVersion(previous, sizeof(previous))) {
        snprintf(previous, sizeof(previous), "%s", d.cfg.firmware_version);
    }
    snprintf(p.previous, sizeof(p.previous), "%s", previous);

    int64_t epoch = 0;
    if (d.clock.utc(epoch)) p.started_at_s = static_cast<uint32_t>(epoch);
    p.deadline_s = p.started_at_s + d.cfg.ota.rollback_deadline_s;

    FWUP_LOGI("ota", "SHA-256 confere, particao marcada; reiniciando para %s",
              p.version);
    d.store->savePending(p);
    d.setOtaState(OtaState::PendingReboot, AbortReason::None);

    // Publish the state change before rebooting so the server sees it.
    delay(200);
    esp_restart();
}

static void otaStep(FirmwareUpdater::Impl& d, uint32_t now) {
    switch (d.state) {
        case DeviceState::EvaluatingOrder: {
            if (!otaLinkReady(d)) {
                // Spec: abort before downloading and re-evaluate on Wi-Fi. The
                // order stays retained, so it fires again by itself.
                d.setOtaState(OtaState::Aborted, AbortReason::Gprs);
                d.state = DeviceState::Operation;
                return;
            }

            const bool waived = d.order.mandatory && d.cfg.ota.mandatory_waives_button;
            const bool needs_button =
                d.cfg.ota.require_button && d.order.requires_button && !waived;

            if (!needs_button || d.cfg.button.button_pin < 0) {
                startDownload(d, now);
                return;
            }

            d.button_deadline_ms =
                now + static_cast<uint32_t>(d.remote_cfg.ota_button_window_s) * 1000u;
            d.state = DeviceState::AwaitingButton;
            d.setOtaState(OtaState::PendingUser, AbortReason::None);
            return;
        }

        case DeviceState::AwaitingButton:
            if (buttonPressed(d)) {
                startDownload(d, now);
            } else if (static_cast<int32_t>(now - d.button_deadline_ms) >= 0) {
                // Stays pending_user and the order stays retained, so it comes
                // back on the next boot.
                d.has_order = false;
                d.state     = DeviceState::Operation;
            }
            return;

        case DeviceState::Downloading:
            if (!otaLinkReady(d)) {
                failOta(d, AbortReason::Network);
                return;
            }
            pumpDownload(d, now);
            return;

        case DeviceState::Verifying:
            verifyAndApply(d, now);
            return;

        default:
            return;
    }
}

// --------------------------------------------------------------- confirm ---

static void confirmStep(FirmwareUpdater::Impl& d, uint32_t now) {
    if (!linkUp() || !d.confirm_backoff.ready(now)) return;

    DeviceStore::Pending p;
    if (!d.store->loadPending(p)) {
        d.state = DeviceState::Operation;
        return;
    }

    // Bootloader rollback triggers on reset, not on a hang, so an image that
    // boots and never confirms needs this deadline to be recovered.
    int64_t epoch = 0;
    if (p.deadline_s > 0 && d.clock.utc(epoch) &&
        static_cast<uint32_t>(epoch) > p.deadline_s) {
        FWUP_LOGE("ota", "prazo de confirmacao esgotado: revertendo para a "
                         "particao anterior");
        d.store->clearPending();
        d.ota.markInvalidAndReboot();
        return;
    }

    char base[128] = {};
    char url[kMaxUrlLen] = {};
    if (!apiBase(d, base, sizeof(base))) return;
    if (!url::formatPath(base, d.cfg.endpoints.path_confirm, p.update_id,
                         url, sizeof(url))) {
        return;
    }

    char body[384] = {};
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"status\":\"success\",\"firmware_version\":\"%s\","
             "\"repo\":\"%s\",\"previous_version\":\"%s\",\"duration_ms\":%u}",
             d.device_id, p.version, p.repo, p.previous, 0u);

    HttpResponse res;
    if (d.http.postJson(url, body, d.scratch, kJsonScratch, res) != HttpError::Ok ||
        res.status != 200) {
        FWUP_LOGW("ota", "confirmacao falhou (HTTP %d), nova tentativa em %u ms",
                  res.status, d.confirm_backoff.currentDelayMs());
        d.confirm_backoff.fail(now);
        return;
    }

    // Spec section 9.2: only after the 200 does the device record the version
    // and cancel the rollback.
    FWUP_LOGI("ota", "confirmado pelo servidor: gravando versao e cancelando rollback");
    d.store->commitFirmware(p.version, p.repo);
    d.store->setLastUpdateId(p.update_id);
    d.ota.markValid();
    d.store->clearPending();

    d.confirm_backoff.reset();
    d.state = DeviceState::Operation;
    d.setOtaState(OtaState::Idle, AbortReason::None);
}

// ------------------------------------------------------------------ ping ---

static void pingStep(FirmwareUpdater::Impl& d, uint32_t now) {
    if (!d.mqtt->connected() || !d.topics.ready()) return;

    const uint32_t interval = static_cast<uint32_t>(d.remote_cfg.ping_interval_s) * 1000u;
    if (d.last_ping_ms != 0 && (now - d.last_ping_ms) < interval) return;
    d.last_ping_ms = now;

    char version[kMaxVersionLen] = {};
    if (!d.store->firmwareVersion(version, sizeof(version))) {
        snprintf(version, sizeof(version), "%s", d.cfg.firmware_version);
    }

    PingSnapshot s;
    int64_t epoch = 0;
    if (d.clock.utc(epoch)) {
        s.ts        = epoch;
        s.ts_source = d.clock.source();
    }
    s.firmware_version = version;
    s.repo             = d.cfg.repo;
    s.uptime_s         = now / 1000u;
    s.link             = linkUp() ? LinkType::Wifi : LinkType::None;
    s.rssi             = linkUp() ? static_cast<int16_t>(WiFi.RSSI()) : 0;
    s.free_heap        = ESP.getFreeHeap();
    s.ota_state        = d.ota_state;
    s.abort_reason     = d.abort_reason;
    s.config_version   = d.store->configVersion();
    s.hardware_model   = d.hardware_model;
    s.pub_qos          = d.caps.mqtt_max_publish_qos;
    s.mqtt_dropped     = d.mqtt->dropped();

    char out[kPingScratch] = {};
    const bool dual = (d.cfg.link_mode == LinkMode::Both);
    if (d.ping_builder.build(s, dual, out, sizeof(out)) != CodecError::Ok) return;

    const bool ok = d.mqtt->publish(d.topics.ping(),
                                   reinterpret_cast<const uint8_t*>(out),
                                   strlen(out), 1, false);
    if (ok) {
        FWUP_LOGD("ping", "%s", out);
    } else {
        FWUP_LOGW("ping", "publish falhou");
    }
}

// ------------------------------------------------------------------ loop ---

void FirmwareUpdater::loop() {
    Impl& d = *_impl;
    if (d.store == nullptr) return;

    const uint32_t now = millis();
    d.clock.tick(now);

    if (d.state == DeviceState::Confirming) {
        confirmStep(d, now);
        return;
    }

    if (!d.store->isProvisioned()) {
        provisionStep(d, now);
        return;
    }

    mqttStep(d, now);
    ingestStep(d);
    otaStep(d, now);
    pingStep(d, now);
}

// ---------------------------------------------------------------- status ---

bool FirmwareUpdater::isProvisioned() const {
    return _impl->store != nullptr && _impl->store->isProvisioned();
}

DeviceState  FirmwareUpdater::state() const    { return _impl->state; }
OtaState     FirmwareUpdater::otaState() const { return _impl->ota_state; }
Capabilities FirmwareUpdater::capabilities() const { return _impl->caps; }

bool FirmwareUpdater::deviceId(char* out, size_t cap) const {
    if (out == nullptr || cap == 0) return false;
    snprintf(out, cap, "%s", _impl->device_id);
    return _impl->device_id[0] != '\0';
}

bool FirmwareUpdater::isBusy() const {
    switch (_impl->state) {
        case DeviceState::Downloading:
        case DeviceState::Verifying:
        case DeviceState::Applying:
            return true;
        default:
            return false;
    }
}

SleepBlock FirmwareUpdater::sleepBlock() const {
    switch (_impl->state) {
        case DeviceState::Provisioning:   return SleepBlock::Provisioning;
        // Sleeping through the window would make the button unpressable.
        case DeviceState::AwaitingButton: return SleepBlock::AwaitingButton;
        case DeviceState::Downloading:    return SleepBlock::Downloading;
        case DeviceState::Verifying:      return SleepBlock::Verifying;
        case DeviceState::Applying:       return SleepBlock::Applying;
        // The rollback deadline is running and a sleeping device cannot confirm.
        case DeviceState::Confirming:     return SleepBlock::PendingConfirm;
        default:                          return SleepBlock::None;
    }
}

bool FirmwareUpdater::canSleep() const {
    return sleepBlock() == SleepBlock::None;
}

// ------------------------------------------------------------------ mqtt ---

bool FirmwareUpdater::mqttConnected() const { return _impl->mqtt->connected(); }
MqttStatus FirmwareUpdater::mqttStatus() const { return _impl->mqtt_status; }
const char* FirmwareUpdater::clientId() const { return _impl->client_id; }

bool FirmwareUpdater::publish(const char* topic, const uint8_t* payload, size_t len,
                              uint8_t qos, bool retain) {
    Impl& d = *_impl;
    if (topic == nullptr) return false;

    if (!d.mqtt->connected()) {
        d.mqtt_status = MqttStatus::NotConnected;
        FWUP_LOGW("mqtt", "publish em %s descartado: offline", topic);
        return false;
    }
    if (len > d.cfg.mqtt.buffer_bytes) {
        d.mqtt_status = MqttStatus::PayloadTooLarge;
        FWUP_LOGE("mqtt", "publish em %s descartado: %u bytes > buffer de %u",
                  topic, (unsigned)len, d.cfg.mqtt.buffer_bytes);
        return false;
    }

    uint8_t effective = qos;
    if (effective > d.caps.mqtt_max_publish_qos) {
        effective     = d.caps.mqtt_max_publish_qos;
        d.mqtt_status = MqttStatus::QosDowngraded;
        FWUP_LOGW("mqtt", "qos %u indisponivel neste transporte, usando %u",
                  qos, effective);
    } else {
        d.mqtt_status = MqttStatus::Ok;
    }

    const bool ok = d.mqtt->publish(topic, payload, len, effective, retain);
    if (!ok) {
        if (d.mqtt_status == MqttStatus::Ok) d.mqtt_status = MqttStatus::Failed;
        FWUP_LOGE("mqtt", "publish em %s falhou (%u bytes)", topic, (unsigned)len);
    } else {
        FWUP_LOGD("mqtt", "-> %s  %u bytes  qos %u%s", topic, (unsigned)len,
                  effective, retain ? " retido" : "");
    }
    return ok;
}

bool FirmwareUpdater::publish(const char* topic, const char* payload,
                              uint8_t qos, bool retain) {
    if (payload == nullptr) return false;
    return publish(topic, reinterpret_cast<const uint8_t*>(payload),
                   strlen(payload), qos, retain);
}

bool FirmwareUpdater::publish(const char* topic, const String& payload,
                              uint8_t qos, bool retain) {
    return publish(topic, payload.c_str(), qos, retain);
}

bool FirmwareUpdater::subscribe(const char* topic, uint8_t qos) {
    Impl& d = *_impl;
    if (topic == nullptr || topic[0] == '\0') return false;

    // Remembered so a reconnect reapplies it without the project doing anything.
    if (d.subs_count < Impl::kMaxSubs) {
        bool known = false;
        for (size_t i = 0; i < d.subs_count && !known; ++i) {
            known = (strcmp(d.subs[i], topic) == 0);
        }
        if (!known) {
            snprintf(d.subs[d.subs_count], kMaxTopicLen, "%s", topic);
            d.subs_qos[d.subs_count] = qos;
            ++d.subs_count;
        }
    }

    return d.mqtt->connected() ? d.mqtt->subscribe(topic, qos) : true;
}

bool FirmwareUpdater::subscribe(const String& topic, uint8_t qos) {
    return subscribe(topic.c_str(), qos);
}

bool FirmwareUpdater::unsubscribe(const char* topic) {
    Impl& d = *_impl;
    if (topic == nullptr) return false;

    for (size_t i = 0; i < d.subs_count; ++i) {
        if (strcmp(d.subs[i], topic) != 0) continue;
        for (size_t j = i; j + 1 < d.subs_count; ++j) {
            memcpy(d.subs[j], d.subs[j + 1], kMaxTopicLen);
            d.subs_qos[j] = d.subs_qos[j + 1];
        }
        --d.subs_count;
        break;
    }
    return true;
}

bool FirmwareUpdater::appTopic(const char* suffix, char* out, size_t cap) const {
    Impl& d = *_impl;
    if (out == nullptr || cap == 0 || d.cfg.topics.project_slug == nullptr) return false;

    const char* key = d.device_id[0] != '\0' ? d.device_id : d.board_id;
    const int n = (suffix == nullptr || suffix[0] == '\0')
                      ? snprintf(out, cap, "%s/ret/%s", d.cfg.topics.project_slug, key)
                      : snprintf(out, cap, "%s/ret/%s/%s", d.cfg.topics.project_slug,
                                 key, suffix);
    return n > 0 && static_cast<size_t>(n) < cap;
}

// ------------------------------------------------------------- callbacks ---

void FirmwareUpdater::onMessage(MqttMessageCb cb, void* ctx) {
    _impl->cb_msg_ctx = ctx;
    _impl->cb_message = cb;
}
void FirmwareUpdater::onMqttConnect(MqttEventCb cb, void* ctx) {
    _impl->cb_conn_ctx = ctx;
    _impl->cb_connect  = cb;
}
void FirmwareUpdater::onMqttDisconnect(MqttEventCb cb, void* ctx) {
    _impl->cb_disc_ctx   = ctx;
    _impl->cb_disconnect = cb;
}
void FirmwareUpdater::onPingExtend(PingExtendCb cb, void* ctx) {
    _impl->cb_ping_ctx = ctx;
    _impl->cb_ping     = cb;
    _impl->ping_builder.setExtender(cb, ctx);
}
void FirmwareUpdater::onRemoteConfig(RemoteConfigCb cb, void* ctx) {
    _impl->cb_config_ctx = ctx;
    _impl->cb_config     = cb;
}
void FirmwareUpdater::onOtaState(OtaStateCb cb, void* ctx) {
    _impl->cb_ota_ctx = ctx;
    _impl->cb_ota     = cb;
}
void FirmwareUpdater::onOrderVeto(OrderVetoCb cb, void* ctx) {
    _impl->cb_veto_ctx = ctx;
    _impl->cb_veto     = cb;
}

// ------------------------------------------------------------ production ---

void FirmwareUpdater::setLogging(bool enabled) { log::setEnabled(enabled); }
void FirmwareUpdater::setLogLevel(LogLevel level) { log::setLevel(level); }
bool FirmwareUpdater::logging() const { return log::enabled(); }

bool FirmwareUpdater::seedBootstrapUrl(const char* url) {
    return _impl->store != nullptr && _impl->store->seedBootstrapUrl(url);
}

bool FirmwareUpdater::factoryReset() {
    return _impl->store != nullptr && _impl->store->factoryReset();
}

}  // namespace campodata

#endif  // FWUP_TARGET_ARDUINO
