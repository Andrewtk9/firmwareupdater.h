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

// O manual AT da SIMCom da ate 75 s ao CIPSTART em modo multi-conexao. Enquanto
// o modem responder CONNECTING, a abertura e aguardada ate aqui, sem bloquear a
// task e sem fechar o socket.
constexpr uint32_t kCipstartMaxMs = 75000;

// Com o CIPSTART ainda em curso, de quanto em quanto tempo o modem e consultado.
constexpr uint32_t kReavaliarAberturaMs = 1000;

// Depois que o modem confirma CONNECTED contra o TinyGSM, a confirmacao vale por
// este tempo antes de ser refeita: limita o trafego AT enquanto o TinyGSM nao se
// ressincroniza.
constexpr uint32_t kReconfirmarMs = 2000;

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

// ---------------------------------------------------------------- SocketMqttGsm

int SocketMqttGsm::connect(IPAddress ip, uint16_t port) {
    TinyGsmClient* s = _link.mqttClient();
    _aberto        = (s != nullptr) && s->connect(ip, port);
    _confirmado_ms = 0;
    return _aberto ? 1 : 0;
}

int SocketMqttGsm::connect(const char* host, uint16_t port) {
    TinyGsmClient* s = _link.mqttClient();
    _aberto        = (s != nullptr) && s->connect(host, port);
    _confirmado_ms = 0;
    return _aberto ? 1 : 0;
}

size_t SocketMqttGsm::write(uint8_t c) {
    return write(&c, 1);
}

size_t SocketMqttGsm::write(const uint8_t* buf, size_t size) {
    return _link.enviarMqtt(buf, size);
}

int SocketMqttGsm::available() {
    TinyGsmClient* s = _link.mqttClient();
    return (s != nullptr) ? s->available() : 0;
}

int SocketMqttGsm::read() {
    TinyGsmClient* s = _link.mqttClient();
    return (s != nullptr) ? s->read() : -1;
}

int SocketMqttGsm::read(uint8_t* buf, size_t size) {
    TinyGsmClient* s = _link.mqttClient();
    return (s != nullptr) ? s->read(buf, size) : -1;
}

int SocketMqttGsm::peek() {
    TinyGsmClient* s = _link.mqttClient();
    return (s != nullptr) ? s->peek() : -1;
}

void SocketMqttGsm::flush() {
    TinyGsmClient* s = _link.mqttClient();
    if (s != nullptr) s->flush();
}

void SocketMqttGsm::stop() {
    TinyGsmClient* s = _link.mqttClient();
    // O stop() padrao do TinyGSM espera ate 15 s drenando o buffer.
    if (s != nullptr) s->stop(1500);
    _aberto        = false;
    _confirmado_ms = 0;
}

uint8_t SocketMqttGsm::connected() {
    TinyGsmClient* s = _link.mqttClient();
    if (s == nullptr) {
        _aberto = false;
        return 0;
    }

    if (s->connected()) {
        _aberto        = true;
        _confirmado_ms = 0;
        return 1;
    }

    // Ja estava fechado: nada a confirmar, e nenhum AT a mais com a sessao caida.
    if (!_aberto) return 0;

    // O TinyGSM acabou de dar por fechado um socket que estava aberto. Ele decide
    // isso com um AT+CIPSTATUS de 1 s: se a resposta atrasa, o socket vivo vira
    // fechado. Antes de o PubSubClient declarar a sessao perdida e mandar
    // CIPCLOSE, o modem e consultado com prazo folgado.
    const uint32_t agora = millis();
    if (_confirmado_ms != 0 && agora - _confirmado_ms < kReconfirmarMs) return 1;

    if (_link.estadoSocketMqtt() == GsmLink::EstadoSocket::Conectado) {
        if (_confirmado_ms == 0) {
            FWUP_LOGW("mqtt", "TinyGSM deu o socket por fechado, mas o modem responde CONNECTED; sessao mantida");
        }
        _confirmado_ms = agora;
        return 1;
    }

    _aberto        = false;
    _confirmado_ms = 0;
    return 0;
}

// ---------------------------------------------------------------- GsmMqttClient

GsmMqttClient::GsmMqttClient(GsmLink& link) : _link(link), _socket(link), _cliente() {
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

    // O PubSubClient fala com o adaptador, e nao direto com o TinyGsmClient:
    // ver SocketMqttGsm.
    _cliente.setClient(_socket);
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
    if (_conectado) _cliente.disconnect();
    _estava_conectado = false;   // desligamento pedido, nao queda
    _conectado        = false;
    _cipstart_ms      = 0;
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
    // Um bool, e nunca uma consulta ao modem.
    //
    // O PubSubClient::connected() desce ate o TinyGsmClient, cujo available()
    // manda AT+CIPRXGET e AT+CIPSTATUS pela UART. Chamado de outra task - no
    // projeto, o Core 0 perguntando se o MQTT estava de pe -, isso colocava dois
    // dialogos AT na mesma UART ao mesmo tempo. Uma resposta embaralhada fazia o
    // PubSubClient concluir que o TCP tinha caido e mandar AT+CIPCLOSE num
    // socket vivo: a sessao caia em estado -3 com o modem ainda em CONNECTED, em
    // momentos que pareciam aleatorios - eram as colisoes. Nos logs, o Core 0
    // chegava a anunciar "MQTT fora" antes da propria task do modem perceber.
    //
    // Quem consulta o modem e so o loop(), uma vez por volta.
    return _conectado;
}

GsmMqttClient::Tentativa GsmMqttClient::conectar() {
    if (!_link.up() || _link.httpBusy()) return Tentativa::Falhou;

    // Abre o TCP aqui, com prazo proprio: o PubSubClient pula o connect dele
    // quando o socket ja esta aberto.
    TinyGsmClient* socket = _link.mqttClient();
    if (socket == nullptr) return Tentativa::Falhou;

    const uint32_t pdp = _link.pdpSeq();
    const bool     pdp_novo = (pdp != _pdp_da_tentativa);
    _pdp_da_tentativa = pdp;
    const int prazo = pdp_novo ? kConnectTimeoutNovoS : kConnectTimeoutS;

    // Uma abertura pendente pertence ao PDP em que foi pedida.
    if (pdp_novo) _cipstart_ms = 0;

    if (socket->connected()) {
        _cipstart_ms = 0;
    } else if (_cipstart_ms != 0) {
        // Um CIPSTART anterior ainda sem desfecho: nada de abrir outro por cima.
        const Tentativa t = avaliarAbertura(socket);
        if (t != Tentativa::Conectou) return t;
    } else {
        // Fecha o que tiver sobrado do socket anterior com prazo curto. O stop()
        // padrao do TinyGSM espera ate 15 s drenando o buffer.
        socket->stop(1500);
        _cipstart_ms    = millis();
        _avisou_abrindo = false;
        if (socket->connect(_host, _cfg.port, prazo)) {
            _cipstart_ms = 0;
        } else {
            const Tentativa t = avaliarAbertura(socket);
            if (t != Tentativa::Conectou) return t;
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
        return Tentativa::Falhou;
    }

    _link.reportMqttConnected();
    _estava_conectado = true;
    _conectado        = true;
    FWUP_LOGI("mqtt", "conectado a %s:%u", _cfg.host, _cfg.port);

    // As assinaturas NAO sao refeitas aqui. O FirmwareUpdater ja as refaz na
    // transicao para conectado (applySubscriptions), e fazer nos dois lugares
    // mandava dois SUBSCRIBE por topico a cada reconexao. Cada SUBSCRIBE faz o
    // broker reentregar o retido: os 3 KB de waypoints chegavam duas vezes, em
    // 2G, no instante mais fragil da sessao - visto no log como dois
    // "wp uuid match" com 1,4 s de diferenca.

    return Tentativa::Conectou;
}

GsmMqttClient::Tentativa GsmMqttClient::avaliarAbertura(TinyGsmClient* socket) {
    // O connect() do TinyGSM desistiu no prazo curto, mas o CIPSTART pode nao ter
    // acabado: o manual da SIMCom da ate 75 s a ele em modo multi-conexao. E o
    // connect() comeca por um CIPCLOSE - tentar de novo fechava o CONNECT OK que
    // chegasse atrasado, e uma conexao que tinha dado certo virava "nao abriu".
    // Antes de contar falha, o modem diz em que pe o socket esta.
    const uint32_t decorrido = millis() - _cipstart_ms;

    if (decorrido < kCipstartMaxMs) {
        switch (_link.estadoSocketMqtt()) {
            case GsmLink::EstadoSocket::Conectado:
                // O TinyGSM ainda o tem por fechado. available() o faz reler o
                // CIPSTATUS; se nem assim, o adaptador vale pela confirmacao do
                // modem - senao o PubSubClient chamaria connect(), e com ele o
                // CIPCLOSE.
                socket->available();
                _socket.confirmarAberto();
                FWUP_LOGI("mqtt", "TCP com %s:%u abriu depois do prazo (%lu s); aproveitando o socket",
                          _host, _cfg.port, (unsigned long)(decorrido / 1000));
                _cipstart_ms = 0;
                return Tentativa::Conectou;

            case GsmLink::EstadoSocket::Abrindo:
                if (!_avisou_abrindo) {
                    _avisou_abrindo = true;
                    FWUP_LOGI("mqtt", "TCP com %s:%u ainda abrindo apos %lu s; aguardando sem fechar (ate %lu s)",
                              _host, _cfg.port, (unsigned long)(decorrido / 1000),
                              (unsigned long)(kCipstartMaxMs / 1000));
                }
                return Tentativa::Aguardando;

            default:
                break;
        }
    }

    FWUP_LOGW("mqtt", "TCP com %s:%u nao abriu em %lu s", _host, _cfg.port,
              (unsigned long)(decorrido / 1000));
    _cipstart_ms = 0;
    socket->stop(1500);
    _link.reportMqttTransportFailure(millis());
    return Tentativa::Falhou;
}

void GsmMqttClient::loop(uint32_t now) {
    if (!_configurado || _suspenso) return;

    // A unica consulta ao modem sobre a sessao. PubSubClient::loop() ja
    // comeca por connected(), serve o keepalive e devolve se a sessao segue de
    // pe - chamar connected() antes dele so repetia o AT.
    _conectado = _cliente.loop();
    if (_conectado) {
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
        _proxima_tentativa = now;   // a v1 retentava na hora; o backoff so vale entre tentativas
        FWUP_LOGW("mqtt", "sessao caiu (estado %d); reconectando agora",
                  _cliente.state());
        _link.reportMqttSessionLost(now);
        return;
    }

    // Link fora do ar ou emprestado ao HTTP: nao ha o que tentar, e isto nao e
    // falha da sessao. Contar aqui inflava o backoff ate 30 s justamente
    // enquanto o link refazia o PDP ou reiniciava o modem, somando meio minuto
    // a cada recuperacao - e ainda alimentava a escada com falhas que nao
    // diziam nada sobre o modem.
    if (!_link.up() || _link.httpBusy()) {
        _cipstart_ms = 0;   // um CIPSTART pendente nao sobrevive ao link
        return;
    }

    if (now < _proxima_tentativa) return;

    const Tentativa t = conectar();
    if (t == Tentativa::Conectou) {
        _falhas = 0;
        return;
    }
    if (t == Tentativa::Aguardando) {
        // O CIPSTART ainda nao acabou: olha de novo em pouco tempo, sem contar
        // falha e sem mexer no backoff.
        _proxima_tentativa = millis() + kReavaliarAberturaMs;
        return;
    }

    const uint32_t espera = kBackoffMs[_falhas < 4 ? _falhas : 3];
    if (_falhas < 4) _falhas++;
    // millis(), e nao now: now foi lido antes de conectar(), que bloqueia ate o
    // prazo do TCP. Somado a um now de 12 s atras, o backoff ja nascia vencido e
    // a tentativa seguinte saia na hora.
    _proxima_tentativa = millis() + espera;
}

void GsmMqttClient::suspend() {
    if (_suspenso) return;
    _suspenso = true;
    _estava_conectado = false;   // HTTP pediu o link: desconexao de proposito
    if (_conectado) _cliente.disconnect();
    _conectado   = false;
    _cipstart_ms = 0;
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

    // O proprio PubSubClient confere a conexao por dentro; conferir aqui com
    // _cliente.connected() era um AT a mais por chamada.
    if (!_conectado) return false;
    return _cliente.subscribe(topic, qos > 1 ? 1 : qos);
}

bool GsmMqttClient::publish(const char* topic, const uint8_t* payload, size_t len,
                            uint8_t qos, bool retain) {
    (void)qos;   // ver maxPublishQos(): PubSubClient publica sempre em QoS 0
    // Idem: um AT+CIPRXGET/CIPSTATUS a menos por publish.
    if (!_conectado || topic == nullptr) return false;
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
