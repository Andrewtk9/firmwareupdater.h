#include "platform/esp32/Esp32Hardware.h"

#if FWUP_TARGET_ESP32

#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_flash.h>
#include <stdio.h>
#include <string.h>

namespace campodata {
namespace platform {
namespace {

const char* chipName(esp_chip_model_t model) {
    switch (model) {
        case CHIP_ESP32:   return "ESP32";
        case CHIP_ESP32S2: return "ESP32-S2";
        case CHIP_ESP32S3: return "ESP32-S3";
        case CHIP_ESP32C3: return "ESP32-C3";
        default:           return "ESP32-UNKNOWN";
    }
}

uint32_t psramBytes() {
    // heap_caps_get_total_size(MALLOC_CAP_SPIRAM) works on both IDF 4.4 and
    // 5.x. esp_psram_get_size() is 5.x-only and esp_spiram_get_size() is
    // private in 4.4, so neither is portable across the two targets.
    return static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
}

uint32_t flashBytes() {
    uint32_t size = 0;
    if (esp_flash_get_physical_size(esp_flash_default_chip, &size) != ESP_OK) return 0;
    return size;
}

const char* buildBoardName() {
#if defined(ARDUINO_BOARD)
    return ARDUINO_BOARD;
#elif defined(CONFIG_IDF_TARGET)
    return CONFIG_IDF_TARGET;
#else
    return "unknown";
#endif
}

}  // namespace

bool probeHardware(HardwareInfo& out) {
    out = HardwareInfo{};

    esp_chip_info_t info{};
    esp_chip_info(&info);
    snprintf(out.chip, sizeof(out.chip), "%s", chipName(info.model));
    out.cores    = info.cores;
    out.revision = info.revision;

    out.flash_bytes = flashBytes();
    out.psram_bytes = psramBytes();

    esp_efuse_mac_get_default(out.mac);
    snprintf(out.board_build, sizeof(out.board_build), "%s", buildBoardName());

    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (next != nullptr) {
        out.ota.size_bytes = next->size;
        out.ota.offset     = next->address;
        out.ota.subtype    = next->subtype;
        out.ota.valid      = true;
        out.ota_capable    = true;
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    out.running_is_factory =
        running != nullptr && running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;

    return out.flash_bytes > 0;
}

}  // namespace platform
}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
