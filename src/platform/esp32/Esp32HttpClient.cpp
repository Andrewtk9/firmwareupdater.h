#include "platform/esp32/Esp32HttpClient.h"

#if FWUP_TARGET_ESP32

// esp_crt_bundle.h is not on the Arduino include path, but the symbol is
// exported from libmbedtls.a, so the prototype is declared here instead of
// forcing every consuming project to add the directory to build_flags.
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);
#include <stdio.h>
#include <string.h>

namespace campodata {

Esp32HttpClient::~Esp32HttpClient() {
    endDownload();
}

void Esp32HttpClient::configure(uint32_t timeout_ms, bool insecure, const char* ca_pem) {
    _timeout_ms = timeout_ms;
    _insecure   = insecure;
    _ca_pem     = ca_pem;
}

esp_http_client_handle_t Esp32HttpClient::make(const char* url,
                                               esp_http_client_method_t method) {
    esp_http_client_config_t cfg = {};
    cfg.url        = url;
    cfg.method     = method;
    cfg.timeout_ms = static_cast<int>(_timeout_ms);
    cfg.keep_alive_enable = false;

    if (_ca_pem != nullptr) {
        cfg.cert_pem = _ca_pem;
    } else if (_insecure) {
        cfg.skip_cert_common_name_check = true;
    } else {
        // The framework ships a full CA bundle, so ordinary public
        // certificates validate without embedding anything.
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    return esp_http_client_init(&cfg);
}

HttpError Esp32HttpClient::postJson(const char* url, const char* body,
                                    char* out, size_t cap, HttpResponse& res) {
    if (url == nullptr || body == nullptr) return HttpError::Transport;

    res = HttpResponse{};
    if (out != nullptr && cap > 0) out[0] = '\0';

    esp_http_client_handle_t c = make(url, HTTP_METHOD_POST);
    if (c == nullptr) return HttpError::Transport;

    esp_http_client_set_header(c, "Content-Type", "application/json");

    const int body_len = static_cast<int>(strlen(body));
    HttpError result   = HttpError::Ok;

    if (esp_http_client_open(c, body_len) != ESP_OK) {
        esp_http_client_cleanup(c);
        return HttpError::ConnectFailed;
    }

    if (esp_http_client_write(c, body, body_len) != body_len) {
        result = HttpError::Transport;
    } else if (esp_http_client_fetch_headers(c) < 0) {
        result = HttpError::Transport;
    } else {
        res.status = esp_http_client_get_status_code(c);
        _status    = res.status;

        if (out != nullptr && cap > 1) {
            const int read = esp_http_client_read(c, out, static_cast<int>(cap - 1));
            const size_t n = (read > 0) ? static_cast<size_t>(read) : 0;
            out[n]       = '\0';
            res.body_len = n;
        }
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return result;
}

HttpError Esp32HttpClient::beginDownload(const char* url, uint32_t range_offset) {
    if (_dl != nullptr) return HttpError::Transport;
    if (url == nullptr) return HttpError::Transport;

    _status         = 0;
    _content_length = 0;
    _resumed_at     = 0;

    _dl = make(url, HTTP_METHOD_GET);
    if (_dl == nullptr) return HttpError::Transport;

    if (range_offset > 0) {
        char range[48];
        snprintf(range, sizeof(range), "bytes=%u-", range_offset);
        esp_http_client_set_header(_dl, "Range", range);
    }

    if (esp_http_client_open(_dl, 0) != ESP_OK) {
        endDownload();
        return HttpError::ConnectFailed;
    }

    const int len = esp_http_client_fetch_headers(_dl);
    _status = esp_http_client_get_status_code(_dl);
    if (len > 0) _content_length = static_cast<uint32_t>(len);

    if (_status == 206) {
        _resumed_at = range_offset;
    } else if (_status == 200) {
        // The server ignored Range and is sending the whole image, so anything
        // already written has to be discarded rather than appended to.
        _resumed_at = 0;
        if (range_offset > 0) {
            endDownload();
            return HttpError::RangeUnsupported;
        }
    } else {
        endDownload();
        return HttpError::BadStatus;
    }

    return HttpError::Ok;
}

HttpError Esp32HttpClient::readChunk(uint8_t* out, size_t cap, size_t& got) {
    got = 0;
    if (_dl == nullptr) return HttpError::Transport;
    if (out == nullptr || cap == 0) return HttpError::Transport;

    if (esp_http_client_is_complete_data_received(_dl)) return HttpError::Eof;

    const int read = esp_http_client_read(_dl, reinterpret_cast<char*>(out),
                                          static_cast<int>(cap));
    if (read < 0) return HttpError::Transport;
    if (read == 0) {
        return esp_http_client_is_complete_data_received(_dl) ? HttpError::Eof
                                                              : HttpError::WouldBlock;
    }

    got = static_cast<size_t>(read);
    return HttpError::Ok;
}

void Esp32HttpClient::endDownload() {
    if (_dl != nullptr) {
        esp_http_client_close(_dl);
        esp_http_client_cleanup(_dl);
        _dl = nullptr;
    }
}

}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
