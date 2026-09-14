#pragma once

#include "campodata/Types.h"

namespace campodata {

struct HttpResponse {
    int    status    = 0;
    size_t body_len  = 0;
};

// HTTP for the two request/response calls and for the streamed download.
//
// The download is deliberately split into begin/read/end so the caller can
// spend a bounded slice of each loop() on it instead of blocking for the whole
// image.
class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    // Body is written into `out`, always NUL-terminated. A body larger than
    // `cap` is truncated but the status is still reported.
    virtual HttpError postJson(const char* url, const char* body,
                               char* out, size_t cap, HttpResponse& res) = 0;

    // `range_offset` > 0 asks the server to resume, and the caller must check
    // resumedAt() afterwards: a server without Range support answers 200 and
    // starts from zero.
    virtual HttpError beginDownload(const char* url, uint32_t range_offset) = 0;

    // Returns WouldBlock when nothing is available yet and Eof at the end.
    virtual HttpError readChunk(uint8_t* out, size_t cap, size_t& got) = 0;

    virtual void endDownload() = 0;

    virtual int      status() const = 0;
    virtual uint32_t contentLength() const = 0;
    virtual uint32_t resumedAt() const = 0;
    virtual bool     downloadOpen() const = 0;

    // Fornece o corpo de um POST em pedacos. Devolve quantos bytes escreveu em
    // `out`, 0 no fim, negativo em erro.
    //
    // E um callback, e nao um buffer, porque o corpo tipico e um arquivo grande
    // que nao cabe na RAM - e porque assim a biblioteca nao precisa saber de
    // onde ele vem.
    using BodyReader = int (*)(uint8_t* out, size_t cap, void* ctx);

    // GET simples para a aplicacao. Corpo truncado em `cap`, sempre terminado
    // em NUL; o status vem em `res` mesmo quando o corpo nao cabe.
    virtual HttpError get(const char* url, char* out, size_t cap,
                          HttpResponse& res) = 0;

    // POST com corpo transmitido em fluxo. `extra_headers` sao linhas prontas
    // POST com corpo transmitido em fluxo. extra_headers sao linhas
    // prontas, cada uma no formato Nome: valor terminada em CRLF.
    // total_len e obrigatorio porque vai no Content-Length, que o
    // servidor precisa antes do corpo.
    // Content-Length, que o servidor precisa antes do corpo.
    virtual HttpError postStream(const char* url, const char* content_type,
                                 const char* extra_headers,
                                 BodyReader reader, void* ctx, uint32_t total_len,
                                 char* out, size_t cap, HttpResponse& res) = 0;

    // Whether this transport can negotiate TLS at all. False on the cellular
    // stack, which is why an https:// order URL has to be checked before use.
    virtual bool supportsTls() const = 0;
};

}  // namespace campodata
