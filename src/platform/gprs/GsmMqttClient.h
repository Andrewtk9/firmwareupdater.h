#pragma once

#if defined(FWUP_ENABLE_GPRS)

#include <Client.h>
#include <PubSubClient.h>

#include "core/interfaces/IMqttClient.h"
#include "platform/gprs/GsmLink.h"

namespace campodata {

// Client entre o PubSubClient e o TinyGsmClient do mux do MQTT.
//
// Abertura, leitura e fechamento seguem pelo TinyGSM. Duas coisas nao, porque
// os prazos de 1 s dele derrubavam sessoes que estavam de pe:
//   - write(): o CIPSEND vai por GsmLink::enviarMqtt, com prazos de 2G.
//   - connected(): quando o TinyGSM da por fechado um socket que estava aberto
//     - basta um AT+CIPSTATUS demorar mais de 1 s -, o modem e consultado com
//     prazo folgado antes de a sessao ser dada por perdida. Sem isso o
//     PubSubClient ia para o estado -3 e mandava CIPCLOSE num socket vivo.
class SocketMqttGsm : public Client {
public:
    explicit SocketMqttGsm(GsmLink& link) : _link(link) {}

    int     connect(IPAddress ip, uint16_t port) override;
    int     connect(const char* host, uint16_t port) override;
    size_t  write(uint8_t c) override;
    size_t  write(const uint8_t* buf, size_t size) override;
    int     available() override;
    int     read() override;
    int     read(uint8_t* buf, size_t size) override;
    int     peek() override;
    void    flush() override;
    void    stop() override;
    uint8_t connected() override;
    operator bool() override { return connected() != 0; }

    // O modem acabou de confirmar CONNECTED por AT+CIPSTATUS, e o TinyGSM pode
    // ainda nao saber.
    void confirmarAberto() {
        _aberto        = true;
        _confirmado_ms = millis();
    }

private:
    GsmLink& _link;
    bool     _aberto        = false;  // o que connected() respondeu por ultimo
    uint32_t _confirmado_ms = 0;      // ultima confirmacao do modem contra o TinyGSM
};

// MQTT over the cellular link, on PubSubClient.
//
// esp-mqtt cannot be used here. In the Arduino build its config struct carries
// only a transport enum, with no slot for a custom transport handle - that field
// only exists from IDF 5.x. Without PPP the modem is not an lwIP interface, so
// there is nothing for esp-mqtt to sit on.
//
// The honest cost is QoS: PubSubClient publishes at QoS 0 only, while the
// specification asks for 1 on all three topics. maxPublishQos() reports 0 so the
// device can state what it actually did instead of claiming otherwise. It is the
// same guarantee the fleet has today on cellular, now declared instead of
// assumed - and it disappears on the Wi-Fi link, which uses esp-mqtt.
//
// There is no TLS either: cellular talks to the broker on 1883 in the clear.
//
// The session is torn down whenever HTTP takes the link, and rebuilt when it is
// handed back. That is the whole reason begin() keeps its configuration.
class GsmMqttClient : public IMqttClient {
public:
    explicit GsmMqttClient(GsmLink& link);
    ~GsmMqttClient() override { end(); }

    bool begin(const MqttSessionConfig& cfg) override;
    void end() override;

    void setDirectCallback(MqttDirectCb cb, void* ctx) override;
    void reserveTopic(const char* topic) override;

    bool connected() const override;

    bool subscribe(const char* topic, uint8_t qos) override;
    bool publish(const char* topic, const uint8_t* payload, size_t len,
                 uint8_t qos, bool retain) override;

    bool poll(char* topic, size_t topic_cap,
              uint8_t* payload, size_t payload_cap, size_t& len) override;

    // PubSubClient only ever publishes QoS 0.
    uint8_t maxPublishQos() const override { return 0; }

    uint32_t dropped() const override { return _dropped; }

    // Stepped from loop(): keeps the session alive and reconnects it.
    void loop(uint32_t now);

    // Called by the link when HTTP needs exclusive access, and again afterwards.
    void suspend();
    void resume();

private:
    static constexpr size_t kFila       = 6;
    static constexpr size_t kTopicoMax  = 128;
    static constexpr size_t kCargaMax   = 512;
    static constexpr size_t kReservados = 4;

    struct Mensagem {
        char    topico[kTopicoMax] = {};
        uint8_t carga[kCargaMax]   = {};
        size_t  len                = 0;
    };

    static void aoReceber(char* topico, uint8_t* carga, unsigned int len);
    void        entregar(const char* topico, const uint8_t* carga, size_t len);
    bool        reservado(const char* topico) const;
    // Desfecho de uma passada de conexao. Aguardando: o CIPSTART ainda nao
    // acabou, e isso nao e falha.
    enum class Tentativa : uint8_t { Conectou, Falhou, Aguardando };
    Tentativa   conectar();
    Tentativa   avaliarAbertura(TinyGsmClient* socket);

    GsmLink&      _link;
    SocketMqttGsm _socket;
    PubSubClient  _cliente;

    MqttSessionConfig _cfg;
    bool _configurado = false;

    // Copias proprias das strings da sessao. _cfg aponta para elas, nunca para
    // o que chegou em begin(): ver o comentario em begin().
    static constexpr size_t kHostMax      = 64;
    static constexpr size_t kUsuarioMax   = 48;
    static constexpr size_t kSenhaMax     = 64;
    static constexpr size_t kClientIdMax  = 64;
    static constexpr size_t kWillCargaMax = 128;

    char _host[kHostMax]             = {};
    char _usuario[kUsuarioMax]       = {};
    char _senha[kSenhaMax]           = {};
    char _client_id[kClientIdMax]    = {};
    char _will_topico[kTopicoMax]    = {};
    char _will_carga[kWillCargaMax]  = {};
    bool _suspenso    = false;

    // Assinaturas refeitas a cada reconexao: a sessao e limpa e o broker nao
    // guarda nada.
    char    _assinaturas[kReservados][kTopicoMax] = {};
    uint8_t _n_assinaturas = 0;

    char    _reservados[kReservados][kTopicoMax] = {};
    uint8_t _n_reservados = 0;

    Mensagem _fila[kFila];
    volatile uint8_t _inicio = 0;
    volatile uint8_t _fim    = 0;
    uint32_t _dropped = 0;

    MqttDirectCb _direto     = nullptr;
    void*        _direto_ctx = nullptr;

    uint32_t _proxima_tentativa = 0;
    uint8_t  _falhas = 0;

    // Separa "a sessao caiu" de "nao conseguiu abrir": a queda e anotada no
    // link e a reconexao comeca no mesmo PDP (ver GsmLink::reportMqttSessionLost).
    bool _estava_conectado = false;

    // Estado da sessao como a ultima volta do loop() o viu. E o que connected()
    // devolve. So loop(), conectar(), suspend() e end() escrevem, todos na task
    // que chama loop() - a dona da UART do modem. Qualquer outra task apenas le.
    volatile bool _conectado = false;

    // PDP em que a ultima tentativa de TCP foi feita (ver GsmLink::pdpSeq).
    uint32_t _pdp_da_tentativa = 0;

    // CIPSTART que estourou o prazo sem desfecho: quando foi pedido. 0 = nenhum.
    // Enquanto houver um, nenhum outro e aberto por cima (ver avaliarAbertura).
    uint32_t _cipstart_ms    = 0;
    bool     _avisou_abrindo = false;
};

}  // namespace campodata

#endif  // FWUP_ENABLE_GPRS
