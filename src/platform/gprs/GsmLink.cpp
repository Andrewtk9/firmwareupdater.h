#include "platform/gprs/GsmLink.h"

#if defined(FWUP_ENABLE_GPRS)

#include "core/Log.h"

namespace campodata {

namespace {
constexpr uint32_t kAtProbeMs   = 1000;
constexpr uint32_t kNetworkMs   = 60000;
constexpr uint32_t kAttachMs    = 25000;
constexpr uint32_t kBackoffMs[] = {2000, 5000, 15000, 30000};
constexpr uint32_t kHealthMs    = 30000;

// Recuperacao, portada da v1 e apertada no tempo: depois de uma queda o aparelho
// nao pode ficar minutos fora do ar.
//
// Sem registro, CREG e sinal sao consultados a cada kNetCheckMs. CREG=3 reinicia
// na hora; sinal bom sem registro por kNetSinalBomMs tambem (v1, Blocos 70 e 75).
constexpr uint32_t kNetCheckMs    = 3000;
constexpr uint32_t kNetSinalBomMs = 20000;

// Depois de um reset por AT o modem responde em 2-3 s. O boot_settle_ms e para
// o arranque frio, com a alimentacao acabando de subir.
constexpr uint32_t kSettleResetMs = 3000;

// Falhas de transporte seguidas da sessao MQTT: a primeira refaz o PDP, a
// segunda reinicia o modem.
constexpr uint8_t kFalhasRefazerPdp = 1;
constexpr uint8_t kFalhasResetModem = 2;

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

        case State::Settling: {
            const uint32_t settle = (_settle_ms != 0) ? _settle_ms : _cfg.boot_settle_ms;
            if (now - _since < settle) return;
            if (now - _since > settle + 20000) {
                fail("modem nao respondeu ao AT", now);
                return;
            }
            if (_modem->testAT(kAtProbeMs)) {
                FWUP_LOGI("gprs", "modem respondeu ao AT");
                _state = State::Init;
                _since = now;
            }
            break;
        }

        case State::Init:
            // init() e quem manda o ATE0. Sem ele o SIM800L devolve o eco de cada
            // comando antes da resposta, e o TinyGSM le o eco no lugar dela: o IP
            // saia como "AT+CIFSR;E010.131..." e o socket do MQTT nao abria.
            // O restart() do TinyGSM segue de fora: dorme com delay() e chama
            // init() por dentro. O reset da recuperacao e o resetModem().
            if (!_modem->init(_cfg.sim_pin)) {
                // Tambem desiste com o SIM ainda acordando. O eco e o que nao pode
                // sobrar ligado, entao ele sai de qualquer jeito.
                FWUP_LOGW("gprs", "init do modem incompleto; desligando o eco direto");
                _modem->sendAT(GF("E0"));
                _modem->waitResponse();
            }
            _state        = State::Network;
            _since        = now;
            _net_check_ms = now;
            FWUP_LOGI("gprs", "aguardando registro na rede");
            break;

        case State::Network: {
            if (_modem->isNetworkConnected()) {
                FWUP_LOGI("gprs", "registrado, rssi %d dBm", (int)rssiDbm());
                _state = State::Attaching;
                _since = now;
                return;
            }
            if (now - _net_check_ms < kNetCheckMs) break;
            _net_check_ms = now;

            const uint32_t decorrido = now - _since;
            const int      creg      = static_cast<int>(_modem->getRegistrationStatus());
            const int      csq       = _modem->getSignalQuality();
            const bool     sinal_bom = (csq >= 15 && csq != 99);   // 99 = desconhecido

            // v1, Bloco 70: registro NEGADO. O modem nao tenta de novo sozinho, e
            // esperar a janela so gastava tempo.
            if (creg == 3) {
                resetModem("registro negado pela operadora (CREG=3)");
                return;
            }
            // v1, Bloco 75: com sinal a rede esta ali; sem registro, quem travou
            // foi o modem. Em campo ele ficou 5 min assim, com sinal 29/31.
            if (sinal_bom && decorrido >= kNetSinalBomMs) {
                FWUP_LOGW("gprs", "sinal %d/31 e sem registro ha %lu s", csq,
                          (unsigned long)(decorrido / 1000));
                resetModem("modem preso com sinal bom");
                return;
            }
            if (decorrido > kNetworkMs) {
                resetModem("sem registro na rede");
                return;
            }
            break;
        }

        case State::Attaching:
            // O PDP caiu, ou a sessao MQTT nao abre em cima dele. O modem costuma
            // se reanexar sozinho, e CGATT=1 com IP presente nao prova nada: sem
            // derrubar tudo, o PDP "voltava" sempre com o mesmo IP e o TCP seguia
            // sem abrir, ate alguem desligar a placa.
            if (_refazer_pdp) {
                _refazer_pdp = false;
                _comparar_ip = true;
                derrubarPdp();
                _modem->gprsConnect(_cfg.apn, _cfg.user, _cfg.pass);
                _since = millis();   // o prazo do PDP comeca depois da derrubada
                break;
            }
            if (_modem->isGprsConnected()) {
                const IPAddress ip     = _modem->localIP();
                const uint32_t  ip_num = static_cast<uint32_t>(ip);

                // Refeito de proposito e voltou igual: o contexto nao foi desfeito.
                // Depois de um reset de verdade nao se compara, porque a operadora
                // pode repetir o endereco e isso viraria um loop de resets.
                if (_comparar_ip && ip_num != 0 && ip_num == _ip_anterior) {
                    _comparar_ip = false;
                    FWUP_LOGW("gprs", "PDP refeito voltou com o mesmo IP %s", ip.toString().c_str());
                    resetModem("PDP refeito voltou com o mesmo IP");
                    return;
                }
                _comparar_ip    = false;
                _ip_anterior    = ip_num;
                _pdp_seq++;
                _failures       = 0;
                _state          = State::Ready;
                _last_health_ms = now;
                FWUP_LOGI("gprs", "PDP ativo, IP %s", ip.toString().c_str());

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
                resetModem("PDP nao subiu");
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
            if (!_modem->isGprsConnected()) {
                _refazer_pdp = true;
                fail("PDP caiu", millis());
            }
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

void GsmLink::reportMqttTransportFailure(uint32_t now) {
    if (_state != State::Ready || _http_leased) return;
    if (_mqtt_falhas_seguidas < 255) _mqtt_falhas_seguidas++;

    if (_mqtt_falhas_seguidas >= kFalhasResetModem) {
        resetModem("MQTT segue sem abrir depois de refazer o PDP");
        return;
    }
    if (_mqtt_falhas_seguidas >= kFalhasRefazerPdp) {
        _refazer_pdp = true;
        fail("MQTT nao abre com o PDP de pe; refazendo o PDP", now);
    }
}

void GsmLink::reportMqttSessionLost(uint32_t now) {
    if (_state != State::Ready || _http_leased) return;
    // Conta como a primeira falha da escada: se o PDP novo tambem nao servir, a
    // proxima falha ja reinicia o modem.
    if (_mqtt_falhas_seguidas < kFalhasRefazerPdp) _mqtt_falhas_seguidas = kFalhasRefazerPdp;
    _refazer_pdp = true;
    fail("sessao MQTT caiu; refazendo o PDP", now);
}

void GsmLink::reportMqttConnected() {
    _mqtt_falhas_seguidas = 0;
    _resets_modem         = 0;
}

void GsmLink::drenarUart() {
    if (_serial == nullptr) return;
    size_t sobra = 0;
    while (_serial->available() && sobra < 2048) {
        _serial->read();
        sobra++;
    }
    if (sobra > 0) FWUP_LOGI("gprs", "%u bytes velhos descartados da UART", (unsigned)sobra);
}

void GsmLink::derrubarPdp() {
    drenarUart();
    // O gprsDisconnect do TinyGSM manda so CIPSHUT e CGATT=0, mas o gprsConnect
    // tambem abre o bearer do SAPBR e ativa o contexto com CGACT. Aqui tudo e
    // desfeito antes de pedir outro PDP. ERROR e esperado no que ja estava
    // fechado: a resposta so e drenada.
    FWUP_LOGI("gprs", "derrubando o PDP por inteiro");
    _modem->sendAT(GF("+SAPBR=0,1"));
    _modem->waitResponse(10000L);
    _modem->sendAT(GF("+CIPSHUT"));
    _modem->waitResponse(20000L, GF("SHUT OK"));
    _modem->sendAT(GF("+CGACT=0,1"));
    _modem->waitResponse(20000L);
    _modem->sendAT(GF("+CGATT=0"));
    _modem->waitResponse(20000L);
}

void GsmLink::resetModem(const char* motivo) {
    // O que a v1 fazia e esta camada tinha deixado de fora. Sem reset, uma pilha
    // IP travada no SIM800L nunca volta: o PDP "sobe" com o mesmo IP, o CIPSTART
    // nao abre e o aparelho so reconecta desligando a placa. Visto em campo: 55
    // minutos em estado -2 ate reiniciar.
    //
    // Nao e o restart() do TinyGSM, que dorme com delay() e chama init() por
    // dentro. O comando sai daqui e o Settling espera o modem voltar e refaz tudo,
    // inclusive o ATE0 e os AT+CIP*, que nao sobrevivem ao reset. O SIM800L sai de
    // fabrica em autobaud: o primeiro AT do Settling ressincroniza.
    if (_pause != nullptr) {
        _pause(_gate_ctx);
        _mqtt_paused_by_fail = true;
    }
    _http_leased          = false;
    _mqtt_falhas_seguidas = 0;
    // Modem recem-reiniciado nao tem PDP velho para derrubar, e depois de reset a
    // operadora pode repetir o IP sem que isso signifique nada.
    _refazer_pdp = false;
    _comparar_ip = false;

    if ((_resets_modem & 1) == 0) {
        FWUP_LOGW("gprs", "%s; reiniciando o modem (AT+CFUN=1,1)", motivo);
        _modem->sendAT(GF("+CFUN=1,1"));
        _modem->waitResponse(10000L);
    } else {
        // Alternado com o reset completo, como na v1: as vezes o radio so sai do
        // estado preso com CFUN=0/1, e nao com CFUN=1,1.
        FWUP_LOGW("gprs", "%s; o reinicio anterior nao resolveu, desligando e religando o radio (CFUN=0/1)", motivo);
        _modem->sendAT(GF("+CFUN=0"));
        _modem->waitResponse(10000L);
        _modem->sendAT(GF("+CFUN=1"));
        _modem->waitResponse(10000L);
        _modem->sendAT(GF("+COPS=0"));   // registro automatico, como na v1
        _modem->waitResponse(10000L);
    }
    if (_resets_modem < 255) _resets_modem++;

    // O modem reiniciado despeja RDY, +CFUN, +CPIN e afins. Lido como resposta
    // de comando, isso desloca todo o dialogo seguinte.
    drenarUart();

    _settle_ms = kSettleResetMs;
    _state     = State::Settling;
    _since     = millis();   // os waitResponse acima podem ter levado segundos
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
