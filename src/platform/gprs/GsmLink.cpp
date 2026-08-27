#include "platform/gprs/GsmLink.h"

#if defined(FWUP_ENABLE_GPRS)

#include "core/Log.h"

namespace campodata {

namespace {
constexpr uint32_t kAtProbeMs   = 1000;
constexpr uint32_t kNetworkMs   = 60000;
constexpr uint32_t kAttachMs    = 30000;
constexpr uint32_t kBackoffMs[] = {2000, 5000, 15000, 30000};
}  // namespace

bool GsmLink::begin(const GprsConfig& cfg) {
    if (cfg.apn == nullptr || cfg.pin_tx < 0 || cfg.pin_rx < 0) {
        FWUP_LOGE("gprs", "configuracao incompleta: apn e pinos sao obrigatorios");
        return false;
    }

    _cfg = cfg;

    static HardwareSerial porta(1);
    porta.begin(cfg.baud, SERIAL_8N1, cfg.pin_rx, cfg.pin_tx);
    _serial = &porta;

    static TinyGsm modem(porta);
    _modem = &modem;

    _http.init(_modem, cfg.mux_http);
    _mqtt.init(_modem, cfg.mux_mqtt);

    power(true);
    _state = State::Settling;
    _since = millis();

    FWUP_LOGI("gprs", "modem ligando, mux http=%u mqtt=%u", cfg.mux_http, cfg.mux_mqtt);
    return true;
}

void GsmLink::setMqttGate(Gate pause, Gate resume, void* ctx) {
    _pause    = pause;
    _resume   = resume;
    _gate_ctx = ctx;
}

void GsmLink::power(bool on) {
    if (_cfg.pin_power_en >= 0) {
        pinMode(_cfg.pin_power_en, OUTPUT);
        digitalWrite(_cfg.pin_power_en, on ? HIGH : LOW);
    }

    if (on && _cfg.pin_pwrkey >= 0) {
        pinMode(_cfg.pin_pwrkey, OUTPUT);
        digitalWrite(_cfg.pin_pwrkey, HIGH);
        delay(10);              // pulso de hardware, nao temporizacao
        digitalWrite(_cfg.pin_pwrkey, LOW);
        delay(1100);            // o SIM800L exige o nivel baixo por ~1 s
        digitalWrite(_cfg.pin_pwrkey, HIGH);
    }
}

void GsmLink::fail(const char* why, uint32_t now) {
    const uint32_t espera = kBackoffMs[_failures < 4 ? _failures : 3];
    if (_failures < 4) _failures++;

    FWUP_LOGW("gprs", "%s; nova tentativa em %lu ms", why, (unsigned long)espera);

    // Derrubar o PDP com o MQTT de pe emitiria CIPSHUT e fecharia todos os mux.
    if (_pause != nullptr) _pause(_gate_ctx);
    _http_leased = false;

    _state    = State::Backoff;
    _next_try = now + espera;
}

void GsmLink::loop(uint32_t now) {
    if (_modem == nullptr) return;

    switch (_state) {
        case State::Off:
            break;

        case State::Settling:
            if (now - _since < _cfg.boot_settle_ms) return;
            if (now - _since > _cfg.boot_settle_ms + 20000) {
                fail("modem nao respondeu ao AT", now);
                return;
            }
            if (_modem->testAT(kAtProbeMs)) {
                FWUP_LOGI("gprs", "modem respondeu ao AT");
                _state = State::Init;
                _since = now;
            }
            break;

        case State::Init:
            if (_cfg.sim_pin != nullptr && _modem->getSimStatus() != 3) {
                _modem->simUnlock(_cfg.sim_pin);
            }
            _state = State::Network;
            _since = now;
            FWUP_LOGI("gprs", "aguardando registro na rede");
            break;

        case State::Network:
            if (_modem->isNetworkConnected()) {
                FWUP_LOGI("gprs", "registrado, rssi %d dBm", (int)rssiDbm());
                _state = State::Attaching;
                _since = now;
                return;
            }
            if (now - _since > kNetworkMs) fail("sem registro na rede", now);
            break;

        case State::Attaching:
            if (_modem->isGprsConnected()) {
                _failures = 0;
                _state    = State::Ready;
                FWUP_LOGI("gprs", "PDP ativo, IP %s",
                          _modem->getLocalIP().c_str());
                return;
            }
            if (now - _since > kAttachMs) {
                fail("PDP nao subiu", now);
                return;
            }
            _modem->gprsConnect(_cfg.apn, _cfg.user, _cfg.pass);
            break;

        case State::Ready:
            // Uma queda com o HTTP em curso e tratada por quem detem o lease.
            if (!_http_leased && !_modem->isGprsConnected()) {
                fail("PDP caiu", now);
            }
            break;

        case State::Backoff:
            if (now < _next_try) return;
            _state = State::Network;
            _since = now;
            break;
    }
}

bool GsmLink::acquireForHttp() {
    if (_state != State::Ready || _http_leased) return false;

    // O MQTT sai de cena antes de o HTTP encostar no link. Nunca os dois juntos.
    if (_pause != nullptr) _pause(_gate_ctx);

    _http_leased = true;
    FWUP_LOGD("gprs", "link cedido ao HTTP");
    return true;
}

void GsmLink::releaseHttp() {
    if (!_http_leased) return;

    _http.stop();   // fecha apenas o mux do HTTP
    _http_leased = false;

    FWUP_LOGD("gprs", "link devolvido ao MQTT");
    if (_resume != nullptr) _resume(_gate_ctx);
}

int16_t GsmLink::rssiDbm() {
    if (_modem == nullptr) return 0;

    const int16_t csq = _modem->getSignalQuality();
    if (csq == 99 || csq < 0) return 0;      // 99 = desconhecido
    return static_cast<int16_t>(-113 + 2 * csq);
}

bool GsmLink::networkTime(int& year, int& month, int& day, int& hour, int& minute,
                          int& second, int8_t& tz_quarter_hours) {
    if (_modem == nullptr) return false;

    float tz = 0;
    if (!_modem->getNetworkTime(&year, &month, &day, &hour, &minute, &second, &tz)) {
        return false;
    }

    // getNetworkTime devolve o fuso em horas; o formato do modem e em quartos.
    tz_quarter_hours = static_cast<int8_t>(tz * 4.0f);
    return year > 2000;
}

}  // namespace campodata

#endif  // FWUP_ENABLE_GPRS
