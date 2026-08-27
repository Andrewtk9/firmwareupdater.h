#pragma once

#if defined(FWUP_ENABLE_GPRS)

#include <PubSubClient.h>

#include "core/interfaces/IMqttClient.h"
#include "platform/gprs/GsmLink.h"

namespace campodata {

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
    bool        conectar();

    GsmLink&     _link;
    PubSubClient _cliente;

    MqttSessionConfig _cfg;
    bool _configurado = false;
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
};

}  // namespace campodata

#endif  // FWUP_ENABLE_GPRS
