#include "platform/esp32/Sha256.h"

#if FWUP_TARGET_ESP32

#include <mbedtls/version.h>

// mbedtls 3.x dropped the _ret suffix. Arduino 2.0.17 ships 2.28, ESP-IDF 5.x
// ships 3.x, so both spellings have to be reachable from one source tree.
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    #define FWUP_SHA_STARTS(c)       mbedtls_sha256_starts((c), 0)
    #define FWUP_SHA_UPDATE(c, p, n) mbedtls_sha256_update((c), (p), (n))
    #define FWUP_SHA_FINISH(c, o)    mbedtls_sha256_finish((c), (o))
#else
    #define FWUP_SHA_STARTS(c)       mbedtls_sha256_starts_ret((c), 0)
    #define FWUP_SHA_UPDATE(c, p, n) mbedtls_sha256_update_ret((c), (p), (n))
    #define FWUP_SHA_FINISH(c, o)    mbedtls_sha256_finish_ret((c), (o))
#endif

namespace campodata {

Sha256::~Sha256() {
    reset();
}

bool Sha256::begin() {
    reset();
    mbedtls_sha256_init(&_ctx);
    if (FWUP_SHA_STARTS(&_ctx) != 0) {
        mbedtls_sha256_free(&_ctx);
        return false;
    }
    _active = true;
    return true;
}

bool Sha256::update(const uint8_t* data, size_t len) {
    if (!_active || (data == nullptr && len > 0)) return false;
    if (len == 0) return true;
    return FWUP_SHA_UPDATE(&_ctx, data, len) == 0;
}

bool Sha256::finish(uint8_t out[kSha256Bytes]) {
    if (!_active || out == nullptr) return false;

    const bool ok = FWUP_SHA_FINISH(&_ctx, out) == 0;
    mbedtls_sha256_free(&_ctx);
    _active = false;
    return ok;
}

void Sha256::reset() {
    if (_active) {
        mbedtls_sha256_free(&_ctx);
        _active = false;
    }
}

}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
