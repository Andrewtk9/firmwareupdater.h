#include "platform/esp32/EspMqttClient.h"

#if FWUP_TARGET_ESP32

// esp_crt_bundle.h is not on the Arduino include path, but the symbol is
// exported from libmbedtls.a, so the prototype is declared here instead of
// forcing every consuming project to add the directory to build_flags.
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);

#include <mbedtls/ssl.h>

namespace {

// Aceita o certificado do servidor sem validar, para brokers cuja cadeia nao
// pode ser verificada.
//
// esp-tls recusa iniciar sem nenhuma ancora de confianca, e a opcao de
// dispensa-la nao existe: CONFIG_ESP_TLS_INSECURE vem desligado no framework
// pre-compilado. Mas o campo crt_bundle_attach da config recebe justamente o
// mbedtls_ssl_config, e e chamado depois de o esp-tls ter posto
// VERIFY_REQUIRED. Preencher esse ponteiro satisfaz a checagem de "tem ancora"
// e permite baixar o modo em seguida - a mesma coisa que o setInsecure() do
// WiFiClientSecure faz, so que por dentro do esp-mqtt, preservando QoS 1.
extern "C" esp_err_t fwupTlsNoVerify(void* conf) {
    if (conf != nullptr) {
        mbedtls_ssl_conf_authmode(static_cast<mbedtls_ssl_config*>(conf),
                                  MBEDTLS_SSL_VERIFY_NONE);
    }
    return ESP_OK;
}

}  // namespace
#include <stdio.h>
#include <string.h>

#include "core/Log.h"

namespace campodata {

EspMqttClient::~EspMqttClient() {
    end();
}

bool EspMqttClient::begin(const MqttSessionConfig& cfg) {
    if (_client != nullptr) end();
    if (cfg.host == nullptr || cfg.host[0] == '\0') return false;

    esp_mqtt_client_config_t mc = {};
    mc.host      = cfg.host;
    mc.port      = cfg.port;
    mc.client_id = cfg.client_id;
    mc.username  = cfg.username;
    mc.password  = cfg.password;
    mc.transport = cfg.tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;

    mc.keepalive              = cfg.keepalive_s;
    mc.disable_clean_session  = cfg.clean_session ? 0 : 1;
    mc.buffer_size            = cfg.buffer_bytes;
    mc.disable_auto_reconnect = false;

    if (cfg.will_topic != nullptr && cfg.will_payload != nullptr) {
        mc.lwt_topic   = cfg.will_topic;
        mc.lwt_msg     = cfg.will_payload;
        mc.lwt_msg_len = static_cast<int>(strlen(cfg.will_payload));
        mc.lwt_qos     = cfg.will_qos;
        mc.lwt_retain  = cfg.will_retain ? 1 : 0;
    }

    if (cfg.tls) {
        if (cfg.ca_pem != nullptr) {
            // Ancora informada pelo projeto: validacao de verdade.
            mc.cert_pem = cfg.ca_pem;
        } else if (cfg.verify_ca) {
            // Valida contra o bundle de CAs ja compilado no framework, sem
            // precisar embutir certificado nenhum.
            mc.crt_bundle_attach = esp_crt_bundle_attach;
        } else {
            // Sem ancora e sem validar: cifra, mas nao autentica.
            mc.crt_bundle_attach = fwupTlsNoVerify;
        }

        // O certificado do broker pode ter CN que nao corresponde ao host.
        mc.skip_cert_common_name_check = true;
    }

    _client = esp_mqtt_client_init(&mc);
    if (_client == nullptr) return false;

    if (esp_mqtt_client_register_event(_client, MQTT_EVENT_ANY, onEvent, this) != ESP_OK) {
        esp_mqtt_client_destroy(_client);
        _client = nullptr;
        return false;
    }

    if (esp_mqtt_client_start(_client) != ESP_OK) {
        esp_mqtt_client_destroy(_client);
        _client = nullptr;
        return false;
    }

    _started = true;
    return true;
}

void EspMqttClient::end() {
    if (_client == nullptr) return;

    if (_started) esp_mqtt_client_stop(_client);
    esp_mqtt_client_destroy(_client);

    _client     = nullptr;
    _started    = false;
    _connected  = false;
    _assembling = false;
    _ring.clear();
}

void EspMqttClient::setDirectCallback(MqttDirectCb cb, void* ctx) {
    _direct_ctx = ctx;
    _direct     = cb;
}

void EspMqttClient::reserveTopic(const char* topic) {
    if (topic == nullptr || topic[0] == '\0') return;
    if (_reserved_count >= kMaxReserved) return;
    if (isReserved(topic)) return;

    snprintf(_reserved[_reserved_count], kMaxTopicLen, "%s", topic);
    ++_reserved_count;
}

bool EspMqttClient::isReserved(const char* topic) const {
    if (topic == nullptr) return false;
    for (size_t i = 0; i < _reserved_count; ++i) {
        if (strcmp(_reserved[i], topic) == 0) return true;
    }
    return false;
}

// Reserved topics always go through the ring; everything else follows the
// project's choice.
void EspMqttClient::deliver(const char* topic, const uint8_t* payload, size_t len) {
    if (_direct != nullptr && !isReserved(topic)) {
        _direct(topic, payload, len, _direct_ctx);
        return;
    }
    _ring.push(topic, payload, len);
}

bool EspMqttClient::subscribe(const char* topic, uint8_t qos) {
    if (_client == nullptr || topic == nullptr || !_connected) return false;
    return esp_mqtt_client_subscribe(_client, topic, qos) >= 0;
}

bool EspMqttClient::publish(const char* topic, const uint8_t* payload, size_t len,
                            uint8_t qos, bool retain) {
    if (_client == nullptr || topic == nullptr || !_connected) return false;

    const int id = esp_mqtt_client_publish(_client, topic,
                                           reinterpret_cast<const char*>(payload),
                                           static_cast<int>(len), qos, retain ? 1 : 0);
    return id >= 0;
}

bool EspMqttClient::poll(char* topic, size_t topic_cap,
                         uint8_t* payload, size_t payload_cap, size_t& len) {
    len = 0;

    Ring::Message msg;
    if (!_ring.pop(msg)) return false;
    if (msg.length > payload_cap) return false;

    if (topic != nullptr && topic_cap > 0) {
        snprintf(topic, topic_cap, "%s", msg.topic);
    }
    if (payload != nullptr && msg.length > 0) {
        memcpy(payload, msg.payload, msg.length);
    }
    len = msg.length;
    return true;
}

void EspMqttClient::onEvent(void* arg, esp_event_base_t, int32_t, void* data) {
    auto* self = static_cast<EspMqttClient*>(arg);
    if (self != nullptr) self->handle(static_cast<esp_mqtt_event_handle_t>(data));
}

// Runs on the esp-mqtt task. No parsing and no allocation here: copy and leave.
void EspMqttClient::handle(esp_mqtt_event_handle_t event) {
    if (event == nullptr) return;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            _connected = true;
            break;

        case MQTT_EVENT_DISCONNECTED:
            _connected  = false;
            _assembling = false;
            break;

        // Sem isto uma credencial recusada e indistinguivel de rede ruim: o
        // esp-mqtt reconecta sozinho e o dispositivo fica tentando em silencio.
        case MQTT_EVENT_ERROR: {
            const esp_mqtt_error_codes_t* e = event->error_handle;
            if (e == nullptr) break;

            if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                const char* motivo = "recusado";
                switch (e->connect_return_code) {
                    case MQTT_CONNECTION_REFUSE_PROTOCOL:
                        motivo = "versao de protocolo recusada"; break;
                    case MQTT_CONNECTION_REFUSE_ID_REJECTED:
                        motivo = "client id rejeitado"; break;
                    case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
                        motivo = "servidor indisponivel"; break;
                    case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
                        motivo = "usuario ou senha invalidos"; break;
                    case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
                        motivo = "nao autorizado"; break;
                    default: break;
                }
                FWUP_LOGE("mqtt", "conexao recusada pelo broker: %s", motivo);
            } else {
                FWUP_LOGE("mqtt", "erro de transporte: tls=0x%x pilha=0x%x sock=%d",
                          e->esp_tls_last_esp_err, e->esp_tls_stack_err,
                          e->esp_transport_sock_errno);
            }
            break;
        }

        case MQTT_EVENT_DATA: {
            const bool fragmented = event->total_data_len > event->data_len;

            if (!fragmented) {
                deliver(event->topic != nullptr ? event->topic : _assembly_topic,
                        reinterpret_cast<const uint8_t*>(event->data),
                        static_cast<size_t>(event->data_len));
                break;
            }

            // First fragment carries the topic; later ones do not.
            if (event->current_data_offset == 0) {
                _assembling    = true;
                _assembly_len  = 0;
                snprintf(_assembly_topic, sizeof(_assembly_topic), "%s",
                         event->topic != nullptr ? event->topic : "");
            }
            if (!_assembling) break;

            const size_t chunk = static_cast<size_t>(event->data_len);
            if (_assembly_len + chunk > kAssemblyBytes) {
                _assembling = false;   // too big: drop rather than deliver a partial
                break;
            }
            memcpy(_assembly + _assembly_len, event->data, chunk);
            _assembly_len += chunk;

            if (_assembly_len >= static_cast<size_t>(event->total_data_len)) {
                deliver(_assembly_topic, _assembly, _assembly_len);
                _assembling   = false;
                _assembly_len = 0;
            }
            break;
        }

        default:
            break;
    }
}

}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
