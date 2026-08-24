#pragma once

#include "campodata/detail/BuildTargets.h"

#if FWUP_TARGET_ESP32

#include <esp_http_client.h>

#include "core/interfaces/IHttpClient.h"

namespace campodata {

// HTTP over esp_http_client.
//
// Not HTTPClient: this one does Range, TLS and chunked reads natively, and the
// same code compiles under ESP-IDF. It also avoids the whole class of bugs
// where a response body is accumulated one character at a time into a String.
//
// TLS validates against the certificate bundle compiled into the framework, so
// no CA has to be embedded and no connection runs unverified by default.
class Esp32HttpClient final : public IHttpClient {
public:
    Esp32HttpClient() = default;
    ~Esp32HttpClient() override;

    Esp32HttpClient(const Esp32HttpClient&) = delete;
    Esp32HttpClient& operator=(const Esp32HttpClient&) = delete;

    // `insecure` disables certificate validation. Off by default: the fleet's
    // servers present publicly valid certificates, so there is no reason to.
    void configure(uint32_t timeout_ms, bool insecure, const char* ca_pem = nullptr);

    HttpError postJson(const char* url, const char* body,
                       char* out, size_t cap, HttpResponse& res) override;

    HttpError beginDownload(const char* url, uint32_t range_offset) override;
    HttpError readChunk(uint8_t* out, size_t cap, size_t& got) override;
    void      endDownload() override;

    int      status() const override         { return _status; }
    uint32_t contentLength() const override  { return _content_length; }
    uint32_t resumedAt() const override      { return _resumed_at; }
    bool     downloadOpen() const override   { return _dl != nullptr; }
    bool     supportsTls() const override    { return true; }

private:
    esp_http_client_handle_t make(const char* url, esp_http_client_method_t method);

    esp_http_client_handle_t _dl = nullptr;
    uint32_t _timeout_ms    = 15000;
    bool     _insecure      = false;
    const char* _ca_pem     = nullptr;
    int      _status        = 0;
    uint32_t _content_length = 0;
    uint32_t _resumed_at    = 0;
};

}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
