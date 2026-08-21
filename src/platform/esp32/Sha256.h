#pragma once

#include "campodata/detail/BuildTargets.h"

#if FWUP_TARGET_ESP32

#include <mbedtls/sha256.h>

#include "campodata/Types.h"

namespace campodata {

// Streaming SHA-256. Fed as the image is written so verification never needs a
// second pass over the firmware.
class Sha256 {
public:
    Sha256() = default;
    ~Sha256();

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool begin();
    bool update(const uint8_t* data, size_t len);
    bool finish(uint8_t out[kSha256Bytes]);
    void reset();

    bool active() const { return _active; }

private:
    mbedtls_sha256_context _ctx{};
    bool _active = false;
};

}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
