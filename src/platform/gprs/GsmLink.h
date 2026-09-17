#pragma once

#if defined(FWUP_ENABLE_GPRS)

#include <Arduino.h>
#include <TinyGsmClient.h>

#include "campodata/Config.h"

namespace campodata {

// Owns the modem, the PDP context and the rule that HTTP and MQTT never share
// the link.
//
// The rule is not a convention here: HTTP can only run through a lease, and
// taking the lease tears the MQTT session down first. The fleet lost months to
// the other arrangement, where an HTTP stop() emitted AT+CIPCLOSE and took the
// MQTT socket with it, and where reconnecting GPRS issued AT+CIPSHUT, which in
// multi-connection mode closes every mux at once.
//
// Everything is stepped from loop(): bringing a SIM800L up takes seconds, and
// none of it may block the application.
class GsmLink {
public:
    // Called to stop and restart the MQTT session around an HTTP lease.
    using Gate = void (*)(void* ctx);

    bool begin(const GprsConfig& cfg);
    void loop(uint32_t now);

    // PDP context active and usable.
    bool up() const { return _state == State::Ready; }

    // Registered by the owner so the lease can close the session before HTTP
    // touches the link, and open it again afterwards.
    void setMqttGate(Gate pause, Gate resume, void* ctx);

    // Exclusive access for an HTTP exchange. Returns false when the link is not
    // ready. Every acquire must be matched by a release, including on failure.
    bool acquireForHttp();
    void releaseHttp();
    bool httpBusy() const { return _http_leased; }

    // Escada de recuperacao, alimentada pela sessao MQTT.
    //
    // Com o PDP de pe o link nao percebe sozinho uma pilha TCP travada no
    // modem: CGATT continua 1 e o IP continua la, mas nenhum socket abre. So
    // quem tenta abrir sabe. A sessao conta aqui cada falha de TRANSPORTE
    // seguida (recusa do broker nao entra) e o link escala: refaz o PDP e, se
    // nao bastar, reinicia o modem. O primeiro sucesso zera a escada.
    void reportMqttTransportFailure(uint32_t now);
    void reportMqttConnected();

    // A sessao estava de pe e caiu. Nos logs de campo o TCP nunca mais abriu em
    // cima do mesmo PDP depois disso, entao o link refaz o PDP na hora.
    void reportMqttSessionLost(uint32_t now);

    // Muda a cada PDP que sobe. A sessao usa isso para saber se e a primeira
    // tentativa em cima deste contexto - a primeira merece um prazo maior,
    // porque nela cabe a resolucao de nome, que em 2G e lenta.
    uint32_t pdpSeq() const { return _pdp_seq; }

    // A client bound to the HTTP mux. Only valid while the lease is held.
    TinyGsmClient* httpClient() { return _http_leased ? &_http : nullptr; }

    // The MQTT mux, held by the session for as long as it lives.
    TinyGsmClient* mqttClient() { return &_mqtt; }

    // Estado do socket do MQTT segundo o proprio modem (AT+CIPSTATUS=<mux>), com
    // prazo folgado. O TinyGSM faz a mesma consulta, mas espera 1 s e, se a
    // resposta atrasa, conclui que o socket fechou. So na task dona da UART.
    enum class EstadoSocket : uint8_t { Conectado, Abrindo, Outro, SemResposta };
    EstadoSocket estadoSocketMqtt(uint32_t prazo_ms = 3000);

    // AT+CIPSEND no mux do MQTT com os prazos que um enlace 2G pede. Devolve
    // quantos bytes o modem aceitou. Ver o comentario em GsmLink.cpp.
    size_t enviarMqtt(const uint8_t* buf, size_t len);

    int16_t rssiDbm();

    // Identidade do modem e do chip SIM.
    //
    // Nao basta chamar o TinyGSM: neste SIM800L V2 o getIMEI() volta vazio logo
    // depois do begin, e foi por isso que a frota acabou com um leitor manual
    // por AT no projeto. O caminho manual vive aqui agora - varre a resposta
    // atras da sequencia valida, tolerando URC e fim de linha estranho.
    bool imei(char* out, size_t cap);
    bool iccid(char* out, size_t cap);

    // Local time from the network, with the offset the modem reports. Discarding
    // that offset is what silently stored local time as UTC across the fleet.
    bool networkTime(int& year, int& month, int& day, int& hour, int& minute,
                     int& second, int8_t& tz_quarter_hours);

private:
    enum class State : uint8_t {
        Off,        // powered down
        Settling,   // rail is up, modem not answering AT yet
        Init,       // AT works, configuring
        Network,    // waiting for registration
        Attaching,  // opening the PDP context
        Ready,
        Backoff
    };

    void power(bool on);
    void fail(const char* why, uint32_t now);
    void resetModem(const char* motivo);
    void derrubarPdp();

    // Le e joga fora o que sobrou na UART. Resposta atrasada, URC desconhecido
    // ou aviso de boot do modem ficam no buffer e deslocam a leitura seguinte:
    // o TinyGSM passa a ler a resposta do comando anterior. Isso nao se
    // conserta sozinho, sobrevive ao reset do modem, e era o que obrigava a
    // reiniciar a placa.
    void drenarUart();

    GprsConfig _cfg;
    HardwareSerial* _serial = nullptr;
    TinyGsm*        _modem  = nullptr;

    // Constructed over _modem once it exists; see begin().
    TinyGsmClient _http;
    TinyGsmClient _mqtt;

    State    _state      = State::Off;
    uint32_t _since      = 0;
    uint32_t _next_try   = 0;
    uint8_t  _failures   = 0;
    bool     _http_leased = false;

    uint32_t _last_health_ms      = 0;
    bool     _mqtt_paused_by_fail = false;

    // Ver reportMqttTransportFailure().
    uint8_t _mqtt_falhas_seguidas = 0;
    uint8_t _resets_modem         = 0;
    bool    _refazer_pdp          = false;  // proximo Attaching derruba o PDP e pede outro

    // Recuperacao portada da v1 (ver GsmLink.cpp).
    uint32_t _settle_ms    = 0;      // 0 = boot_settle_ms; depois de reset, curto
    uint32_t _net_check_ms = 0;      // ultima consulta de CREG e sinal
    uint32_t _ip_anterior  = 0;      // IP do ultimo PDP que ficou de pe
    uint32_t _pdp_seq      = 0;      // incrementa a cada PDP que sobe
    bool     _comparar_ip  = false;  // PDP refeito de proposito: o IP tem de mudar

    Gate  _pause  = nullptr;
    Gate  _resume = nullptr;
    void* _gate_ctx = nullptr;
};

}  // namespace campodata

#endif  // FWUP_ENABLE_GPRS
