#pragma once

#include "campodata/Types.h"

namespace campodata {

// What the platform layer probes at runtime. Compile-time macros cannot answer
// this: esp32dev and WROVER are the same silicon and differ only by PSRAM, yet
// the spec's hardware_model has to tell them apart.
struct HardwareInfo {
    char        chip[16]        = {};   // "ESP32", "ESP32-S3"
    uint8_t     cores           = 0;
    uint16_t    revision        = 0;
    uint32_t    flash_bytes     = 0;
    uint32_t    psram_bytes     = 0;
    uint8_t     mac[6]          = {};
    char        board_build[32] = {};   // compile-time board name, informational
    OtaSlotInfo ota;
    bool        ota_capable        = false;
    bool        running_is_factory = false;
};

namespace hardware {

// Classifies into the three families the fleet actually runs. Pure function, so
// every branch is unit-testable without an ESP32 present.
//
//   ESP32-S3            -> "esp32s3-n<flashMB>r<psramMB>"
//   ESP32 with PSRAM    -> "esp32-wrover-n<flashMB>r<psramMB>"
//   ESP32 without PSRAM -> "esp32-wroom-n<flashMB>"
bool classify(const HardwareInfo& hw, char* out, size_t cap);

// board_id as the whole fleet and the existing server already key on it:
// 12 uppercase hex from the efuse MAC, no separators.
//
// Note the trap: ESP.getEfuseMac() is byte-reversed relative to
// WiFi.macAddress(), so this string is NOT the printed Wi-Fi MAC.
bool boardId(const uint8_t mac[6], char* out, size_t cap);

}  // namespace hardware
}  // namespace campodata
