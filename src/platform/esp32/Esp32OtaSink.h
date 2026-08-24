#pragma once

#include "campodata/detail/BuildTargets.h"

#if FWUP_TARGET_ESP32

#include <esp_ota_ops.h>

#include "core/interfaces/IOtaSink.h"
#include "platform/esp32/Sha256.h"

namespace campodata {

// Writes firmware straight to the inactive OTA slot via esp_ota_ops.
//
// Deliberately not Update.h: we need the digest streamed as we write, an
// explicit abort, control over when the image becomes bootable, and the same
// code path under both frameworks.
class Esp32OtaSink final : public IOtaSink {
public:
    Esp32OtaSink() = default;
    ~Esp32OtaSink() override;

    Esp32OtaSink(const Esp32OtaSink&) = delete;
    Esp32OtaSink& operator=(const Esp32OtaSink&) = delete;

    bool         probe(OtaSlotInfo& out) const override;
    OtaSinkError begin(uint32_t expected_size) override;
    OtaSinkError write(const uint8_t* data, size_t len) override;
    uint32_t     written() const override { return _written; }
    OtaSinkError finish(const uint8_t expected_sha256[kSha256Bytes]) override;
    void         abort() override;

    bool pendingVerify() const override;
    bool markValid() override;
    bool markInvalidAndReboot() override;

    // Running image's version string, used for previous_version in the confirm.
    static bool runningVersion(char* out, size_t cap);

private:
    const esp_partition_t* _target  = nullptr;
    esp_ota_handle_t       _handle  = 0;
    uint32_t               _written = 0;
    uint32_t               _expected = 0;
    bool                   _open    = false;
    Sha256                 _sha;
};

}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
