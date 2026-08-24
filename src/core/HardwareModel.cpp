#include "core/HardwareModel.h"

#include <stdio.h>
#include <string.h>

namespace campodata {
namespace hardware {
namespace {

uint32_t toMegabytes(uint32_t bytes) {
    return bytes / (1024u * 1024u);
}

bool isS3(const char* chip) {
    return strstr(chip, "S3") != nullptr || strstr(chip, "s3") != nullptr;
}

bool isPlainEsp32(const char* chip) {
    // "ESP32", "ESP32-D0WD-V3", "ESP32-U4WDH" all classify as the original part.
    return strncmp(chip, "ESP32", 5) == 0 && !isS3(chip) &&
           strstr(chip, "S2") == nullptr && strstr(chip, "C3") == nullptr &&
           strstr(chip, "C6") == nullptr && strstr(chip, "H2") == nullptr;
}

void lowercaseCopy(const char* src, char* dst, size_t cap) {
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; ++i) {
        const char c = src[i];
        dst[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    dst[i] = '\0';
}

}  // namespace

bool classify(const HardwareInfo& hw, char* out, size_t cap) {
    if (!out || cap == 0) return false;

    const uint32_t flash_mb = toMegabytes(hw.flash_bytes);
    const uint32_t psram_mb = toMegabytes(hw.psram_bytes);

    int n = -1;
    if (isS3(hw.chip)) {
        n = (psram_mb > 0)
                ? snprintf(out, cap, "esp32s3-n%ur%u", flash_mb, psram_mb)
                : snprintf(out, cap, "esp32s3-n%u", flash_mb);
    } else if (isPlainEsp32(hw.chip)) {
        // PSRAM is the only thing separating a WROVER from a plain esp32dev.
        n = (psram_mb > 0)
                ? snprintf(out, cap, "esp32-wrover-n%ur%u", flash_mb, psram_mb)
                : snprintf(out, cap, "esp32-wroom-n%u", flash_mb);
    } else {
        char lower[sizeof(hw.chip)] = {};
        lowercaseCopy(hw.chip, lower, sizeof(lower));
        n = snprintf(out, cap, "%s-n%u", lower, flash_mb);
    }

    return n > 0 && static_cast<size_t>(n) < cap;
}

bool boardId(const uint8_t mac[6], char* out, size_t cap) {
    if (!mac || !out || cap < 13) return false;

    // Byte order matches the fleet's sprintf("%04X%08X", hi16, lo32) of
    // ESP.getEfuseMac(), which reverses the octets. Changing it would orphan
    // every device already registered on the server.
    const int n = snprintf(out, cap, "%02X%02X%02X%02X%02X%02X",
                           mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
    return n == 12;
}

}  // namespace hardware
}  // namespace campodata
