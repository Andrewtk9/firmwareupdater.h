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

    // A client bound to the HTTP mux. Only valid while the lease is held.
    TinyGsmClient* httpClient() { return _http_leased ? &_http : nullptr; }

    // The MQTT mux, held by the session for as long as it lives.
    TinyGsmClient* mqttClient() { return &_mqtt; }

    int16_t rssiDbm();

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

    Gate  _pause  = nullptr;
    Gate  _resume = nullptr;
    void* _gate_ctx = nullptr;
};

}  // namespace campodata

#endif  // FWUP_ENABLE_GPRS
