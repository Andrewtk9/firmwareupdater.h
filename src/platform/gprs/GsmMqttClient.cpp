#include "platform/gprs/GsmMqttClient.h"

#if defined(FWUP_ENABLE_GPRS)

#include <string.h>

#include "core/Log.h"

namespace campodata {

namespace {

// PubSubClient entrega no callback sem contexto, entao a instancia viva precisa
// ser alcancavel de fora. Ha uma sessao por dispositivo, nunca duas.
GsmMqttClient* g_instancia = nullptr;

constexpr uint32_t kBackoffMs[] = {3000, 8000, 15000, 30000};

// Os defaults do PubSubClient sao numeros de rede cabeada: em 2G o PINGRESP
// passa de 15 s e o cliente se desconecta sozinho achando que o broker sumiu.
constexpr uint16_t kSocketTimeoutS = 30;

}  // namespace

GsmMqttClient::GsmMqttClient(GsmLink& link) : _link(link), _cliente() {
    g_instancia = this;
}

void GsmMqttClient::aoReceber(char* topico, uint8_t* carga, unsigned int len) {
    if (g_instancia != nullptr) {
        g_instancia->entregar(topico, carga, len);
    }
}

bool GsmMqttClient::reservado(const char* topico) const {
    for (uint8_t i = 0; i < _n_reservados; i++) {
        if (strcmp(_reservados[i], topico) == 0) return true;
    }
    return false;
}

void GsmMqttClient::entregar(const char* topico, const uint8_t* carga, size_t len) {
    // Os topicos da biblioteca sempre passam pela fila, para que a maquina de
    // estados continue tendo um dono so, qualquer que seja o modo da aplicacao.
    if (_direto != nullptr && !reservado(topico)) {
        _direto(topico, carga, len, _direto_ctx);
        return;
    }

    const uint8_t proximo = static_cast<uint8_t>((_fim + 1) % kFila);
    if (proximo == _inicio) {
        _dropped++;
        FWUP_LOGW("mqtt", "fila cheia, mensagem descartada (%lu no total)",
                  (unsigned long)_dropped);
        return;
    }

    Mensagem& m = _fila[_fim];
    snprintf(m.topico, sizeof(m.topico), "%s", topico);
    m.len = (len < kCargaMax) ? len : kCargaMax;
    memcpy(m.carga, carga, m.len);

    _fim = proximo;
}

bool GsmMqttClient::begin(const MqttSessionConfig& cfg) {
    if (cfg.host == nullptr || cfg.client_id == nullptr) return false;

    if (cfg.tls) {
        // O provisionamento devolveu perfil TLS para um link que nao faz TLS.
        // Seguir adiante daria uma falha de handshake sem explicacao.
        FWUP_LOGE("mqtt", "perfil TLS recebido, mas o GPRS so fala 1883 em claro");
        return false;
    }

    _cfg         = cfg;
    _configurado = true;
    _suspenso    = false;

    TinyGsmClient* socket = _link.mqttClient();
    if (socket == nullptr) return false;

    _cliente.setClient(*socket);
    _cliente.setServer(cfg.host, cfg.port);
    _cliente.setCallback(aoReceber);
    _cliente.setKeepAlive(cfg.keepalive_s);
    _cliente.setSocketTimeout(kSocketTimeoutS);

    // Degradar em silencio para 256 bytes e o que faz mensagens sumirem sem
    // nenhum erro no meio do caminho.
    if (!_cliente.setBufferSize(cfg.buffer_bytes)) {
        FWUP_LOGW("mqtt", "sem memoria para buffer de %u bytes, mantendo o padrao",
                  (unsigned)cfg.buffer_bytes);
    }

    _n_assinaturas     = 0;
    _proxima_tentativa = 0;
    _falhas            = 0;

    FWUP_LOGI("mqtt", "sessao GPRS configurada para %s:%u", cfg.host, cfg.port);
    return true;
}

void GsmMqttClient::end() {
    if (_cliente.connected()) _cliente.disconnect();
    _configurado = false;
    if (g_instancia == this) g_instancia = nullptr;
}

void GsmMqttClient::setDirectCallback(MqttDirectCb cb, void* ctx) {
    _direto     = cb;
    _direto_ctx = ctx;
}

void GsmMqttClient::reserveTopic(const char* topic) {
    if (topic == nullptr || _n_reservados >= kReservados) return;
    snprintf(_reservados[_n_reservados], kTopicoMax, "%s", topic);
    _n_reservados++;
}

bool GsmMqttClient::connected() const {
    return const_cast<PubSubClient&>(_cliente).connected();
}

bool GsmMqttClient::conectar() {
    if (!_link.up() || _link.httpBusy()) return false;

    const bool ok = (_cfg.will_topic != nullptr)
                        ? _cliente.connect(_cfg.client_id, _cfg.username, _cfg.password,
                                           _cfg.will_topic, _cfg.will_qos,
                                           _cfg.will_retain, _cfg.will_payload,
                                           _cfg.clean_session)
                        : _cliente.connect(_cfg.client_id, _cfg.username, _cfg.password);

    if (!ok) {
        FWUP_LOGW("mqtt", "conexao recusada, estado %d", _cliente.state());
        return false;
    }

    FWUP_LOGI("mqtt", "conectado a %s:%u", _cfg.host, _cfg.port);

    // Sessao limpa: o broker nao guarda assinatura nenhuma entre conexoes.
    for (uint8_t i = 0; i < _n_assinaturas; i++) {
        if (!_cliente.subscribe(_assinaturas[i], 1)) {
            FWUP_LOGW("mqtt", "falha ao reassinar %s", _assinaturas[i]);
        }
    }

    return true;
}

void GsmMqttClient::loop(uint32_t now) {
    if (!_configurado || _suspenso) return;

    if (_cliente.connected()) {
        _cliente.loop();     // fora de qualquer condicao: e o keepalive
        _falhas = 0;
        return;
    }

    if (now < _proxima_tentativa) return;

    if (conectar()) {
        _falhas = 0;
        return;
    }

    const uint32_t espera = kBackoffMs[_falhas < 4 ? _falhas : 3];
    if (_falhas < 4) _falhas++;
    _proxima_tentativa = now + espera;
}

void GsmMqttClient::suspend() {
    if (_suspenso) return;
    _suspenso = true;
    if (_cliente.connected()) _cliente.disconnect();
    FWUP_LOGD("mqtt", "sessao suspensa: o link foi para o HTTP");
}

void GsmMqttClient::resume() {
    if (!_suspenso) return;
    _suspenso          = false;
    _proxima_tentativa = 0;   // religa na proxima passada, sem esperar
    FWUP_LOGD("mqtt", "sessao liberada para reconectar");
}

bool GsmMqttClient::subscribe(const char* topic, uint8_t qos) {
    if (topic == nullptr) return false;

    bool ja = false;
    for (uint8_t i = 0; i < _n_assinaturas; i++) {
        if (strcmp(_assinaturas[i], topic) == 0) { ja = true; break; }
    }
    if (!ja && _n_assinaturas < kReservados) {
        snprintf(_assinaturas[_n_assinaturas], kTopicoMax, "%s", topic);
        _n_assinaturas++;
    }

    if (!_cliente.connected()) return false;
    return _cliente.subscribe(topic, qos > 1 ? 1 : qos);
}

bool GsmMqttClient::publish(const char* topic, const uint8_t* payload, size_t len,
                            uint8_t qos, bool retain) {
    (void)qos;   // ver maxPublishQos(): PubSubClient publica sempre em QoS 0
    if (!_cliente.connected() || topic == nullptr) return false;
    return _cliente.publish(topic, payload, len, retain);
}

bool GsmMqttClient::poll(char* topic, size_t topic_cap,
                         uint8_t* payload, size_t payload_cap, size_t& len) {
    len = 0;
    if (_inicio == _fim) return false;

    const Mensagem& m = _fila[_inicio];

    if (topic != nullptr && topic_cap > 0) snprintf(topic, topic_cap, "%s", m.topico);
    len = (m.len < payload_cap) ? m.len : payload_cap;
    if (payload != nullptr) memcpy(payload, m.carga, len);

    _inicio = static_cast<uint8_t>((_inicio + 1) % kFila);
    return true;
}

}  // namespace campodata

#endif  // FWUP_ENABLE_GPRS
