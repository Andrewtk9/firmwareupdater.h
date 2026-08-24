#pragma once

#include "campodata/detail/BuildTargets.h"

#if FWUP_TARGET_ESP32

#include "core/HardwareModel.h"

namespace campodata {
namespace platform {

// Fills HardwareInfo from runtime sources only. Compile-time macros cannot
// distinguish esp32dev from WROVER, and one binary should be able to run on
// both and still report itself correctly.
bool probeHardware(HardwareInfo& out);

}  // namespace platform
}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
