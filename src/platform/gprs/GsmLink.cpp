#include "platform/gprs/GsmLink.h"

#if defined(FWUP_ENABLE_GPRS)

#include "core/Log.h"

namespace campodata {

namespace {
constexpr uint32_t kAtProbeMs   = 1000;
constexpr uint32_t kNetworkMs   = 60000;
constexpr uint32_t kAttachMs    = 30000;
constexpr uint32_t kBackoffMs[] = {2000, 5000, 15000, 30000};
constexpr uint32_t kHealthMs    = 30000;

#if defined(FWUP_GSM_TRACE_AT)
// Espelha na Serial o que passa pela UART do modem, nos dois sentidos. So para
// diagnostico: o volume e alto, e os pacotes MQTT aparecem crus no meio.
class EspelhoAt : public Stream {
public:
    EspelhoAt(Stream& modem, Print& saida) : _modem(modem), _saida(saida) {}

    int available() override { return _modem.available(); }
    int peek() override { return _modem.peek(); }

    int read() override {
        const int c = _modem.read();
        if (c >= 0) {
            marcar(false);
            _saida.write(static_cast<uint8_t>(c));
        }
        return c;
    }

    size_t write(uint8_t c) override {
        marcar(true);
        _saida.write(c);
        return _modem.write(c);
    }

    size_t write(const uint8_t* buf, size_t n) override {
        marcar(true);
        _saida.write(buf, n);
        return _modem.write(buf, n);
    }

    void flush() override { _modem.flush(); }

private:
    // Um marcador a cada troca de sentido basta para ler o dialogo.
    void marcar(bool envio) {
        const uint8_t sentido = envio ? 1 : 2;
        if (_sentido == sentido) return;
        _sentido = sentido;
        _saida.print(envio ? "\n[AT>] " : "\n[AT<] ");
    }

    Stream& _modem;
    Print&  _saida;
    uint8_t _sentido = 0;
};
#endif
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

#if defined(FWUP_GSM_TRACE_AT)
    static EspelhoAt espelho(porta, Serial);
    static TinyGsm modem(espelho);
    FWUP_LOGW("gprs", "trafego AT espelhado na Serial (FWUP_GSM_TRACE_AT)");
#else
    static TinyGsm modem(porta);
#endif
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
    if (_pause != nullptr) {
        _pause(_gate_ctx);
        _mqtt_paused_by_fail = true;
    }
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
            // init() e quem manda o ATE0. Sem ele o SIM800L devolve o eco de cada
            // comando antes da resposta, e o TinyGSM le o eco no lugar dela: o IP
            // saia como "AT+CIFSR;E010.131..." e o socket do MQTT nao abria.
            // restart() fica de fora de proposito: reinicia o modem e perde o baud.
            if (!_modem->init(_cfg.sim_pin)) {
                // Tambem desiste com o SIM ainda acordando. O eco e o que nao pode
                // sobrar ligado, entao ele sai de qualquer jeito.
                FWUP_LOGW("gprs", "init do modem incompleto; desligando o eco direto");
                _modem->sendAT(GF("E0"));
                _modem->waitResponse();
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
                _failures       = 0;
                _state          = State::Ready;
                _last_health_ms = now;
                FWUP_LOGI("gprs", "PDP ativo, IP %s",
                          _modem->getLocalIP().c_str());

                // Quem pausou a sessao ao declarar a queda devolve ela aqui. O
                // resume so existia no fim do HTTP, e um aparelho parado nao faz
                // HTTP: uma unica queda deixava o MQTT suspenso ate reiniciar.
                if (_mqtt_paused_by_fail) {
                    _mqtt_paused_by_fail = false;
                    if (_resume != nullptr) _resume(_gate_ctx);
                }
                return;
            }
            if (now - _since > kAttachMs) {
                fail("PDP nao subiu", now);
                return;
            }
            _modem->gprsConnect(_cfg.apn, _cfg.user, _cfg.pass);
            break;

        case State::Ready:
            // Checar o PDP custa um AT+CGATT? e um AT+CIFSR que espera ate 10 s.
            // Feito a cada volta do loop, ocupava a UART sem parar, e cada CIFSR
            // que estourava virava uma queda que nao existiu. Socket do MQTT
            // aberto ja prova que o PDP esta de pe; uma queda com o HTTP em curso
            // e tratada por quem detem o lease.
            if (_http_leased || _mqtt.connected()) break;
            if (now - _last_health_ms < kHealthMs) break;
            _last_health_ms = now;

            // millis(), e nao now: a checagem pode ter bloqueado por segundos.
            if (!_modem->isGprsConnected()) fail("PDP caiu", millis());
            break;

        case State::Backoff:
            if (now < _next_try) return;
            // Volta pelo init: um modem que reiniciou volta com o eco ligado.
            _state = State::Init;
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

namespace {

// Copia a primeira sequencia de digitos com exatamente `exatos` caracteres.
// Zero em `exatos` aceita qualquer sequencia com pelo menos `minimo`.
bool extrairSequencia(const String& resposta, char* out, size_t cap,
                      size_t exatos, size_t minimo, bool aceita_f) {
    String d;

    for (size_t i = 0; i < resposta.length(); i++) {
        const char c = resposta[i];
        const bool digito = (c >= 0x30 && c <= 0x39) ||
                            (aceita_f && (c == 0x46 || c == 0x66));

        if (digito) {
            d += c;
            continue;
        }

        if (exatos > 0 && d.length() == exatos) break;
        if (exatos == 0 && d.length() >= minimo) break;
        d = "";
    }

    const bool ok = (exatos > 0) ? (d.length() == exatos) : (d.length() >= minimo);
    if (!ok || d.length() + 1 > cap) return false;

    snprintf(out, cap, "%s", d.c_str());
    return true;
}

}  // namespace

bool GsmLink::imei(char* out, size_t cap) {
    if (_modem == nullptr || out == nullptr || cap == 0) return false;
    out[0] = 0;

    const String direto = _modem->getIMEI();
    if (direto.length() == 15 && direto.length() + 1 <= cap) {
        snprintf(out, cap, "%s", direto.c_str());
        return true;
    }

    String resposta;
    _modem->sendAT("+GSN");
    if (_modem->waitResponse(2000L, resposta) != 1) return false;

    return extrairSequencia(resposta, out, cap, 15, 0, false);
}

bool GsmLink::iccid(char* out, size_t cap) {
    if (_modem == nullptr || out == nullptr || cap == 0) return false;
    out[0] = 0;

    const String direto = _modem->getSimCCID();
    if (direto.length() >= 18 && direto.length() + 1 <= cap) {
        snprintf(out, cap, "%s", direto.c_str());
        return true;
    }

    String resposta;
    _modem->sendAT("+CCID");
    if (_modem->waitResponse(2000L, resposta) != 1) return false;

    // O ICCID costuma ter 19 ou 20 caracteres e as vezes termina em F.
    return extrairSequencia(resposta, out, cap, 0, 18, true);
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
