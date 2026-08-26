#pragma once

#if defined(FWUP_ENABLE_GPRS)

#include "core/interfaces/IHttpClient.h"
#include "platform/gprs/GsmLink.h"

namespace campodata {

// HTTP/1.1 over the cellular link, spoken directly into the modem's socket.
//
// esp_http_client is not usable here: it needs an lwIP interface, and there is
// no PPP in the Arduino build, so the modem is only reachable as a stream of AT
// commands. What it costs is TLS - supportsTls() is false, and an https:// URL
// has to be refused before the download starts rather than stalling in the dark.
//
// Every exchange runs under a lease from GsmLink, which takes the MQTT session
// down first: the two never share the link.
class GsmHttpClient : public IHttpClient {
public:
    explicit GsmHttpClient(GsmLink& link) : _link(link) {}
    ~GsmHttpClient() override { endDownload(); }

    HttpError postJson(const char* url, const char* body,
                       char* out, size_t cap, HttpResponse& res) override;

    HttpError beginDownload(const char* url, uint32_t range_offset) override;
    HttpError readChunk(uint8_t* out, size_t cap, size_t& got) override;
    void      endDownload() override;

    int      status() const override { return _status; }
    uint32_t contentLength() const override { return _length; }
    uint32_t resumedAt() const override { return _resumed_at; }
    bool     downloadOpen() const override { return _open; }

    // No TLS on this transport, and pretending otherwise would only move the
    // failure to a place with less context.
    bool supportsTls() const override { return false; }

private:
    struct Alvo {
        char     host[96] = {};
        char     path[192] = {};
        uint16_t port = 80;
    };

    static bool separar(const char* url, Alvo& alvo);

    bool      abrir(const Alvo& alvo, TinyGsmClient*& cliente);
    HttpError lerCabecalhos(TinyGsmClient& cliente, uint32_t range_offset);

    GsmLink& _link;

    int      _status     = 0;
    uint32_t _length     = 0;
    uint32_t _resumed_at = 0;
    uint32_t _recebido   = 0;
    bool     _open       = false;
    uint32_t _ultimo_byte = 0;
};

}  // namespace campodata

#endif  // FWUP_ENABLE_GPRS
