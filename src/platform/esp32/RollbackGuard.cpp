#include "platform/esp32/RollbackGuard.h"

#if FWUP_TARGET_ESP32

#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace campodata {
namespace rollback {

bool supported() {
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    return true;
#else
    return false;
#endif
}

GuardState check(bool update_was_pending) {
    if (!supported()) return GuardState::Unsupported;

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) return GuardState::Ok;

    // A factory partition is never subject to rollback.
    if (running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return GuardState::Ok;

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return GuardState::Ok;

    switch (state) {
        case ESP_OTA_IMG_PENDING_VERIFY:
            // The override held: the decision is still ours to make.
            return GuardState::Held;

        case ESP_OTA_IMG_VALID:
            // Already valid while an update was pending means something marked
            // it before we ran. On Arduino that is initArduino(), which is
            // exactly what the strong symbol is supposed to prevent.
            return update_was_pending ? GuardState::Bypassed : GuardState::Ok;

        default:
            return GuardState::Ok;
    }
}

const char* toString(GuardState s) {
    switch (s) {
        case GuardState::Ok:          return "ok";
        case GuardState::Held:        return "held";
        case GuardState::Bypassed:    return "bypassed";
        case GuardState::Unsupported: return "unsupported";
    }
    return "unknown";
}

}  // namespace rollback
}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
