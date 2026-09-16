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

// Prazo para abrir o TCP do broker. Deixado ao PubSubClient, o connect(host,
// port) do TinyGSM usa 75 s, e cada tentativa que falha congela a task inteira
// esse tempo. Com rede boa o CIPSTART fecha em 1-3 s; 12 s ja e folga, e cada
// segundo aqui e um segundo a mais fora do ar antes de escalar.
constexpr int kConnectTimeoutS = 12;

// Primeira tentativa em cima de um PDP novo: nela cabe a resolucao de nome, que
// em 2G e lenta, e cortar cedo demais faz a escada girar por engano. A v1 usava
// os 75 s do TinyGSM em toda tentativa; aqui o prazo largo vale so uma vez por
// PDP, e as seguintes continuam curtas.
constexpr int kConnectTimeoutNovoS = 45;

// Copia src para dst. Recusa em vez de truncar: um host, usuario ou senha
// cortado daria uma conexao recusada sem nenhuma pista do motivo.
bool copiar(char* dst, size_t cap, const char* src) {
    if (src == nullptr) {
        dst[0] = '\0';
        return true;
    }
    const int n = snprintf(dst, cap, "%s", src);
    return n >= 0 && static_cast<size_t>(n) < cap;
}

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

    // As strings da sessao sao COPIADAS aqui, e nao apenas apontadas.
    //
    // Quem chama monta host, usuario e senha em buffers na propria pilha e
    // retorna logo em seguida; o connect so acontece depois, no loop(). E o
    // PubSubClient::setServer() guarda o ponteiro, nao o texto. Com a pilha ja
    // reaproveitada, o modem recebia lixo no lugar do broker -
    //     AT+CIPSTART=0,"TCP","??5?mode",1883
    // - e usuario e senha iam corrompidos junto. A sessao nunca subia, entao
    // nao saia ping nenhum, e nada disso aparecia como erro.
    //
    // O caminho WiFi nao sofre disso porque o esp-mqtt copia a configuracao
    // dentro do esp_mqtt_client_init, ainda com a pilha de quem chamou viva.
    if (!copiar(_host, sizeof(_host), cfg.host) ||
        !copiar(_usuario, sizeof(_usuario), cfg.username) ||
        !copiar(_senha, sizeof(_senha), cfg.password) ||
        !copiar(_client_id, sizeof(_client_id), cfg.client_id) ||
        !copiar(_will_topico, sizeof(_will_topico), cfg.will_topic) ||
        !copiar(_will_carga, sizeof(_will_carga), cfg.will_payload)) {
        FWUP_LOGE("mqtt", "host, usuario, senha, client id ou will maior que o buffer");
        return false;
    }

    _cfg              = cfg;
    _cfg.host         = _host;
    _cfg.username     = (cfg.username != nullptr) ? _usuario : nullptr;
    _cfg.password     = (cfg.password != nullptr) ? _senha : nullptr;
    _cfg.client_id    = _client_id;
    _cfg.will_topic   = (cfg.will_topic != nullptr) ? _will_topico : nullptr;
    _cfg.will_payload = (cfg.will_payload != nullptr) ? _will_carga : nullptr;
    _cfg.ca_pem       = nullptr;   // sem TLS no celular; nao guardar ponteiro alheio

    _configurado = true;
    _suspenso    = false;

    TinyGsmClient* socket = _link.mqttClient();
    if (socket == nullptr) return false;

    _cliente.setClient(*socket);
    _cliente.setServer(_host, _cfg.port);
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

    FWUP_LOGI("mqtt", "sessao GPRS configurada para %s:%u", _host, _cfg.port);
    return true;
}

void GsmMqttClient::end() {
    if (_cliente.connected()) _cliente.disconnect();
    _estava_conectado = false;   // desligamento pedido, nao queda
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

    // Abre o TCP aqui, com prazo proprio: o PubSubClient pula o connect dele
    // quando o socket ja esta aberto.
    TinyGsmClient* socket = _link.mqttClient();
    if (socket == nullptr) return false;

    const uint32_t pdp = _link.pdpSeq();
    const bool     pdp_novo = (pdp != _pdp_da_tentativa);
    _pdp_da_tentativa = pdp;
    const int prazo = pdp_novo ? kConnectTimeoutNovoS : kConnectTimeoutS;

    if (!socket->connected()) {
        // Fecha o que tiver sobrado do socket anterior com prazo curto. O stop()
        // padrao do TinyGSM espera ate 15 s drenando o buffer.
        socket->stop(1500);
        if (!socket->connect(_host, _cfg.port, prazo)) {
            FWUP_LOGW("mqtt", "TCP com %s:%u nao abriu em %d s", _host, _cfg.port, prazo);
            _link.reportMqttTransportFailure(millis());
            return false;
        }
    }

    const bool ok = (_cfg.will_topic != nullptr)
                        ? _cliente.connect(_cfg.client_id, _cfg.username, _cfg.password,
                                           _cfg.will_topic, _cfg.will_qos,
                                           _cfg.will_retain, _cfg.will_payload,
                                           _cfg.clean_session)
                        : _cliente.connect(_cfg.client_id, _cfg.username, _cfg.password);

    if (!ok) {
        const int estado = _cliente.state();
        FWUP_LOGW("mqtt", "conexao recusada, estado %d", estado);
        // Negativo e transporte: TCP que nao abriu, conexao perdida, CONNACK que
        // nao chegou. Positivo e o broker recusando - usuario, senha, client id -
        // e reiniciar o modem nao mudaria nada.
        if (estado < 0) _link.reportMqttTransportFailure(millis());
        return false;
    }

    _link.reportMqttConnected();
    _estava_conectado = true;
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
        _estava_conectado = true;
        return;
    }

    if (_estava_conectado) {
        // Caiu com a sessao de pe: publish que falhou, keepalive, CLOSED do
        // modem. Como na v1, a primeira providencia e reabrir o socket no mesmo
        // PDP - a escada do link so entra se isso nao der certo. O backoff volta
        // ao inicio para que a retentativa saia em 3 s, e nao nos 30 s em que
        // ele possa ter parado na queda anterior.
        _estava_conectado  = false;
        _falhas            = 0;
        _proxima_tentativa = now + kBackoffMs[0];
        FWUP_LOGW("mqtt", "sessao caiu (estado %d); nova tentativa em %lu ms",
                  _cliente.state(), (unsigned long)kBackoffMs[0]);
        _link.reportMqttSessionLost(now);
        return;
    }

    // Link fora do ar ou emprestado ao HTTP: nao ha o que tentar, e isto nao e
    // falha da sessao. Contar aqui inflava o backoff ate 30 s justamente
    // enquanto o link refazia o PDP ou reiniciava o modem, somando meio minuto
    // a cada recuperacao - e ainda alimentava a escada com falhas que nao
    // diziam nada sobre o modem.
    if (!_link.up() || _link.httpBusy()) return;

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
    _estava_conectado = false;   // HTTP pediu o link: desconexao de proposito
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
