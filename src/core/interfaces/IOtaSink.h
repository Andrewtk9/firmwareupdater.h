#pragma once

#include "campodata/Types.h"

namespace campodata {

// Where firmware bytes go. Kept free of esp_* so the FSM can be driven by a
// fake in native tests.
class IOtaSink {
public:
    virtual ~IOtaSink() = default;

    // Real slot geometry. The only valid source for the size cap: every current
    // variant hardcodes 2000000 while the actual slot is 1966080, so an image
    // in between downloads fully and only then fails to flash.
    virtual bool probe(OtaSlotInfo& out) const = 0;

    virtual OtaSinkError begin(uint32_t expected_size) = 0;

    // Hashes as it writes, so verification costs no second pass over the image.
    virtual OtaSinkError write(const uint8_t* data, size_t len) = 0;

    virtual uint32_t written() const = 0;

    // Verifies the digest and only then makes the image bootable. Doing both
    // here means a caller cannot accidentally boot an unverified image.
    virtual OtaSinkError finish(const uint8_t expected_sha256[kSha256Bytes]) = 0;

    virtual void abort() = 0;

    // Post-boot rollback control.
    virtual bool pendingVerify() const = 0;
    virtual bool markValid() = 0;
    virtual bool markInvalidAndReboot() = 0;
};

}  // namespace campodata
