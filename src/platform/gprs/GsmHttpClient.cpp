#include "platform/gprs/GsmHttpClient.h"

#if defined(FWUP_ENABLE_GPRS)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/Log.h"
#include "core/Url.h"

namespace campodata {

namespace {
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kHeaderTimeoutMs  = 20000;
constexpr uint32_t kIdleTimeoutMs    = 30000;
}  // namespace

bool GsmHttpClient::separar(const char* url, Alvo& alvo) {
    if (url == nullptr) return false;

    const char* p = strstr(url, "://");
    if (p == nullptr) return false;

    if (strncmp(url, "http://", 7) != 0) return false;   // sem TLS aqui
    p += 3;

    const char* barra = strchr(p, '/');
    const char* fim   = (barra != nullptr) ? barra : p + strlen(p);

    const char* doispontos = static_cast<const char*>(memchr(p, ':', fim - p));
    const size_t host_len  = (doispontos != nullptr) ? (size_t)(doispontos - p)
                                                     : (size_t)(fim - p);

    if (host_len == 0 || host_len >= sizeof(alvo.host)) return false;
    memcpy(alvo.host, p, host_len);
    alvo.host[host_len] = '\0';

    alvo.port = (doispontos != nullptr)
                    ? static_cast<uint16_t>(atoi(doispontos + 1))
                    : 80;
    if (alvo.port == 0) return false;

    snprintf(alvo.path, sizeof(alvo.path), "%s", (barra != nullptr) ? barra : "/");
    return true;
}

bool GsmHttpClient::abrir(const Alvo& alvo, TinyGsmClient*& cliente) {
    if (!_link.acquireForHttp()) {
        FWUP_LOGW("http", "link indisponivel para HTTP");
        return false;
    }

    cliente = _link.httpClient();
    if (cliente == nullptr) {
        _link.releaseHttp();
        return false;
    }

    if (!cliente->connect(alvo.host, alvo.port, kConnectTimeoutMs / 1000)) {
        FWUP_LOGW("http", "falha ao conectar em %s:%u", alvo.host, alvo.port);
        _link.releaseHttp();
        return false;
    }

    return true;
}

HttpError GsmHttpClient::lerCabecalhos(TinyGsmClient& cliente, uint32_t range_offset) {
    _status     = 0;
    _length     = 0;
    _resumed_at = 0;
    _recebido   = 0;
    _chunked    = false;
    _chunks.reset();

    const uint32_t limite = millis() + kHeaderTimeoutMs;
    char linha[160];
    bool primeira = true;

    while (millis() < limite) {
        if (!cliente.connected() && cliente.available() == 0) return HttpError::Transport;
        if (cliente.available() == 0) continue;

        const size_t n = cliente.readBytesUntil('\n', linha, sizeof(linha) - 1);
        linha[n] = '\0';
        if (n > 0 && linha[n - 1] == '\r') linha[n - 1] = '\0';

        if (primeira) {
            const char* espaco = strchr(linha, ' ');
            if (espaco == nullptr) return HttpError::BadStatus;
            _status  = atoi(espaco + 1);
            primeira = false;
            continue;
        }

        if (linha[0] == '\0') {                       // fim dos cabecalhos
            if (range_offset > 0 && _status == 200) {
                // O servidor ignorou o Range e vai mandar tudo de novo. Quem
                // chamou precisa saber, para nao concatenar em cima do que ja
                // gravou: resumedAt() fica em zero.
                FWUP_LOGW("http", "servidor sem suporte a Range, reinicio do zero");
            }
            return HttpError::Ok;
        }

        if (strncasecmp(linha, "Content-Length:", 15) == 0) {
            _length = strtoul(linha + 15, nullptr, 10);
        } else if (strncasecmp(linha, "Transfer-Encoding:", 18) == 0) {
            for (const char* p = linha + 18; *p != '\0'; ++p) {
                if (strncasecmp(p, "chunked", 7) == 0) {
                    _chunked = true;
                    break;
                }
            }
        } else if (strncasecmp(linha, "Content-Range:", 14) == 0) {
            const char* traco = strchr(linha, ' ');
            const char* ini   = (traco != nullptr) ? strchr(traco, '=') : nullptr;
            if (ini == nullptr && traco != nullptr) {
                // formato "bytes 1234-5678/9999"
                ini = traco;
                while (*ini == ' ') ini++;
                ini = strpbrk(ini, "0123456789");
            }
            if (ini != nullptr) _resumed_at = strtoul(ini, nullptr, 10);
        }
    }

    return HttpError::Timeout;
}

size_t GsmHttpClient::lerCorpo(TinyGsmClient& cliente, uint8_t* out, size_t cap,
                               bool& fim) {
    fim = false;
    size_t got = 0;

    while (got < cap) {
        // Com os dois cabecalhos, vale o chunked: e o que o servidor de fato
        // mandou no fio.
        if (_chunked) {
            if (_chunks.done() || _chunks.invalid()) {
                fim = true;
                break;
            }
        } else if (_length > 0 && _recebido >= _length) {
            fim = true;
            break;
        }

        const int disponivel = cliente.available();
        if (disponivel <= 0) break;

        // Marcadores de tamanho, CRLF e trailer: um byte por vez, e nenhum deles
        // vai para o corpo.
        if (_chunked && !_chunks.inData()) {
            const int c = cliente.read();
            if (c < 0) break;
            _chunks.feed(static_cast<char>(c));
            continue;
        }

        size_t pedir = cap - got;
        if (static_cast<size_t>(disponivel) < pedir) pedir = static_cast<size_t>(disponivel);
        if (_chunked) {
            if (_chunks.dataRemaining() < pedir) pedir = _chunks.dataRemaining();
        } else if (_length > 0 && (_length - _recebido) < pedir) {
            pedir = _length - _recebido;
        }

        const int lido = cliente.read(out + got, pedir);
        if (lido <= 0) break;

        got       += static_cast<size_t>(lido);
        _recebido += static_cast<uint32_t>(lido);
        if (_chunked) _chunks.consumeData(static_cast<uint32_t>(lido));
    }

    return got;
}

size_t GsmHttpClient::lerResposta(TinyGsmClient& cliente, char* out, size_t cap) {
    uint8_t        bloco[128];
    size_t         total  = 0;
    const uint32_t limite = millis() + kIdleTimeoutMs;

    while (millis() < limite) {
        bool fim = false;
        const size_t n = lerCorpo(cliente, bloco, sizeof(bloco), fim);

        // O que nao cabe e descartado, mas o corpo continua sendo lido ate o fim.
        if (out != nullptr && total + 1 < cap) {
            const size_t cabe = cap - 1 - total;
            memcpy(out + total, bloco, (n < cabe) ? n : cabe);
        }
        total += n;

        if (fim) break;
        if (n == 0 && !cliente.connected() && cliente.available() == 0) break;
    }

    if (_chunked && _chunks.invalid()) {
        FWUP_LOGW("http", "enquadramento chunked invalido: corpo incompleto");
    }

    if (out != nullptr && cap > 0) out[(total < cap) ? total : cap - 1] = '\0';
    return total;
}

HttpError GsmHttpClient::postJson(const char* url, const char* body,
                                  char* out, size_t cap, HttpResponse& res) {
    if (out != nullptr && cap > 0) out[0] = '\0';

    if (!url::fetchable(url, supportsTls())) {
        FWUP_LOGE("http", "URL exige TLS, indisponivel no GPRS: %s", url);
        return HttpError::ConnectFailed;
    }

    Alvo alvo;
    if (!separar(url, alvo)) return HttpError::ConnectFailed;

    TinyGsmClient* cliente = nullptr;
    if (!abrir(alvo, cliente)) return HttpError::ConnectFailed;

    const size_t corpo_len = (body != nullptr) ? strlen(body) : 0;

    cliente->printf("POST %s HTTP/1.1\r\n", alvo.path);
    cliente->printf("Host: %s\r\n", alvo.host);
    cliente->print("Content-Type: application/json\r\n");
    cliente->print("Connection: close\r\n");
    cliente->printf("Content-Length: %u\r\n\r\n", (unsigned)corpo_len);
    if (corpo_len > 0) cliente->write(reinterpret_cast<const uint8_t*>(body), corpo_len);

    HttpError erro = lerCabecalhos(*cliente, 0);
    if (erro != HttpError::Ok) {
        _link.releaseHttp();
        return erro;
    }

    const size_t escrito = lerResposta(*cliente, out, cap);

    res.status   = _status;
    res.body_len = escrito;

    _link.releaseHttp();
    return HttpError::Ok;
}

HttpError GsmHttpClient::get(const char* url, char* out, size_t cap,
                             HttpResponse& res) {
    res = HttpResponse{};
    if (out != nullptr && cap > 0) out[0] = 0;

    if (!url::fetchable(url, supportsTls())) return HttpError::ConnectFailed;

    Alvo alvo;
    if (!separar(url, alvo)) return HttpError::ConnectFailed;

    TinyGsmClient* cliente = nullptr;
    if (!abrir(alvo, cliente)) return HttpError::ConnectFailed;

    cliente->printf("GET %s HTTP/1.1\r\n", alvo.path);
    cliente->printf("Host: %s\r\n", alvo.host);
    cliente->print("Connection: close\r\n\r\n");

    const HttpError erro = lerCabecalhos(*cliente, 0);
    if (erro != HttpError::Ok) {
        _link.releaseHttp();
        return erro;
    }

    const size_t escrito = lerResposta(*cliente, out, cap);

    res.status   = _status;
    res.body_len = escrito;

    _link.releaseHttp();
    return HttpError::Ok;
}

HttpError GsmHttpClient::postStream(const char* url, const char* content_type,
                                    const char* extra_headers,
                                    BodyReader reader, void* ctx,
                                    uint32_t total_len,
                                    char* out, size_t cap, HttpResponse& res) {
    res = HttpResponse{};
    if (out != nullptr && cap > 0) out[0] = 0;

    if (reader == nullptr) return HttpError::Transport;

    if (!url::fetchable(url, supportsTls())) {
        FWUP_LOGE("http", "URL exige TLS, indisponivel no GPRS: %s", url);
        return HttpError::ConnectFailed;
    }

    Alvo alvo;
    if (!separar(url, alvo)) return HttpError::ConnectFailed;

    TinyGsmClient* cliente = nullptr;
    if (!abrir(alvo, cliente)) return HttpError::ConnectFailed;

    cliente->printf("POST %s HTTP/1.1\r\n", alvo.path);
    cliente->printf("Host: %s\r\n", alvo.host);
    cliente->print("Connection: close\r\n");
    cliente->printf("Content-Type: %s\r\n",
                    (content_type != nullptr) ? content_type
                                              : "application/octet-stream");
    cliente->printf("Content-Length: %lu\r\n", (unsigned long)total_len);
    if (extra_headers != nullptr) cliente->print(extra_headers);
    cliente->print("\r\n");

    // Blocos pequenos: o buffer do modem e curto, e um bloco grande so aumenta
    // o tempo que a escrita fica presa sem ceder a CPU.
    uint8_t  buf[256];
    uint32_t enviados = 0;
    bool     ok       = true;

    while (enviados < total_len) {
        const int lido = reader(buf, sizeof(buf), ctx);
        if (lido < 0) { ok = false; break; }
        if (lido == 0) break;

        if (cliente->write(buf, (size_t)lido) != (size_t)lido) { ok = false; break; }
        enviados += (uint32_t)lido;

        // Cede a CPU entre blocos; nao e temporizacao.
        delay(0);
    }

    if (!ok || enviados != total_len) {
        FWUP_LOGE("http", "corpo incompleto: %lu de %lu bytes",
                  (unsigned long)enviados, (unsigned long)total_len);
        _link.releaseHttp();
        return HttpError::Transport;
    }

    const HttpError erro = lerCabecalhos(*cliente, 0);
    if (erro != HttpError::Ok) {
        _link.releaseHttp();
        return erro;
    }

    const size_t escrito = lerResposta(*cliente, out, cap);

    res.status   = _status;
    res.body_len = escrito;

    FWUP_LOGI("http", "POST %s -> %d (%lu bytes enviados)",
              alvo.path, _status, (unsigned long)enviados);

    _link.releaseHttp();
    return HttpError::Ok;
}

HttpError GsmHttpClient::beginDownload(const char* url, uint32_t range_offset) {
    endDownload();

    if (!url::fetchable(url, supportsTls())) {
        FWUP_LOGE("ota", "URL do firmware exige TLS, indisponivel no GPRS");
        return HttpError::ConnectFailed;
    }

    Alvo alvo;
    if (!separar(url, alvo)) return HttpError::ConnectFailed;

    TinyGsmClient* cliente = nullptr;
    if (!abrir(alvo, cliente)) return HttpError::ConnectFailed;

    cliente->printf("GET %s HTTP/1.1\r\n", alvo.path);
    cliente->printf("Host: %s\r\n", alvo.host);
    cliente->print("Accept: application/octet-stream\r\n");
    cliente->print("Connection: close\r\n");
    if (range_offset > 0) {
        cliente->printf("Range: bytes=%lu-\r\n", (unsigned long)range_offset);
    }
    cliente->print("\r\n");

    const HttpError erro = lerCabecalhos(*cliente, range_offset);
    if (erro != HttpError::Ok) {
        _link.releaseHttp();
        return erro;
    }

    if (_status != 200 && _status != 206) {
        FWUP_LOGE("ota", "download recusado, status %d", _status);
        _link.releaseHttp();
        return HttpError::BadStatus;
    }

    _open        = true;
    _recebido    = 0;
    _ultimo_byte = millis();
    return HttpError::Ok;
}

HttpError GsmHttpClient::readChunk(uint8_t* out, size_t cap, size_t& got) {
    got = 0;
    if (!_open || out == nullptr || cap == 0) return HttpError::Transport;

    TinyGsmClient* cliente = _link.httpClient();
    if (cliente == nullptr) return HttpError::Transport;

    bool fim = false;
    got = lerCorpo(*cliente, out, cap, fim);

    // Tamanho que nao e hexadecimal: o que viesse depois iria para a particao
    // como se fosse firmware.
    if (_chunked && _chunks.invalid()) return HttpError::Transport;

    if (got > 0) {
        _ultimo_byte = millis();
        return HttpError::Ok;
    }
    if (fim) return HttpError::Eof;

    if (!cliente->connected() && cliente->available() == 0) {
        // Sem tamanho declarado, a conexao fechar e o fim do corpo. Com
        // Content-Length ou chunked, e corpo truncado.
        return (!_chunked && _length == 0) ? HttpError::Eof : HttpError::Transport;
    }
    if (millis() - _ultimo_byte > kIdleTimeoutMs) return HttpError::Timeout;

    return HttpError::WouldBlock;
}

void GsmHttpClient::endDownload() {
    if (!_open) return;
    _open = false;
    _link.releaseHttp();
}

}  // namespace campodata

#endif  // FWUP_ENABLE_GPRS
