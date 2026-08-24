#pragma once

#include "campodata/detail/BuildTargets.h"

#if FWUP_TARGET_ESP32

#include <stdint.h>

namespace campodata {

// Keeps the OTA rollback window open long enough for the application to use it.
//
// Arduino-ESP32 ships a weak verifyRollbackLater() returning false, and
// initArduino() - which runs before setup() - reacts by calling
// esp_ota_mark_app_valid_cancel_rollback() on any PENDING_VERIFY image. The
// window is therefore closed before a single line of application code runs, and
// the spec's "boot failed -> rollback" branch cannot happen.
//
// The library defines the strong symbol in FirmwareUpdater.cpp so Arduino skips
// that block and the decision stays with us, gated on the confirm response.
namespace rollback {

enum class GuardState : uint8_t {
    Ok,             // no pending image, nothing to guard
    Held,           // pending image still pending: the override is working
    Bypassed,       // pending image already marked valid: the override did NOT link
    Unsupported,    // this build has no rollback support at all
};

// Call once at begin(), after initArduino() has run.
//
// `update_was_pending` comes from NVS and is what makes bypass detectable: an
// image that we know just came from an OTA, yet is already VALID, can only mean
// something confirmed it before us.
//
// A Bypassed result means the strong verifyRollbackLater() was not linked in -
// typically because the linker never extracted its archive member. That failure
// is otherwise completely silent and only shows up in the field as "rollback
// never happened", so it must be reported loudly.
GuardState check(bool update_was_pending);

const char* toString(GuardState);

bool supported();

}  // namespace rollback
}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
