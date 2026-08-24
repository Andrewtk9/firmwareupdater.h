#pragma once

#include "campodata/detail/BuildTargets.h"

#if FWUP_TARGET_ESP32

#include <nvs.h>

#include "core/interfaces/INvs.h"

namespace campodata {

// NVS backed by nvs_flash directly, not by Arduino Preferences.
//
// Preferences is a thin wrapper over this same API but caps key names at 15
// characters and truncates silently past that; going straight to nvs_flash also
// means one implementation serves Arduino and ESP-IDF unchanged.
class Esp32Nvs final : public INvs {
public:
    Esp32Nvs() = default;
    ~Esp32Nvs() override;

    Esp32Nvs(const Esp32Nvs&) = delete;
    Esp32Nvs& operator=(const Esp32Nvs&) = delete;

    // Initialises the partition and opens the namespace. Recovers from a
    // truncated or version-bumped partition by erasing and retrying once,
    // which is the documented handling for NO_FREE_PAGES.
    bool begin(const char* ns = nvskey::kNamespace);
    void end();

    bool getString(const char* key, char* out, size_t cap) const override;
    bool setString(const char* key, const char* value) override;

    bool getU32(const char* key, uint32_t& out) const override;
    bool setU32(const char* key, uint32_t value) override;

    bool getU16(const char* key, uint16_t& out) const override;
    bool setU16(const char* key, uint16_t value) override;

    bool getU8(const char* key, uint8_t& out) const override;
    bool setU8(const char* key, uint8_t value) override;

    bool erase(const char* key) override;
    bool eraseAll() override;
    bool commit() override;

    bool has(const char* key) const override;
    bool encrypted() const override { return _encrypted; }

private:
    nvs_handle_t _handle    = 0;
    bool         _open      = false;
    bool         _encrypted = false;
};

}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
