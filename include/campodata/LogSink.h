#pragma once

#include "campodata/Types.h"

namespace campodata {

enum class LogLevel : uint8_t {
    None = 0,   // silent
    Error,      // something failed and will not retry on its own
    Warn,       // degraded, but still working
    Info,       // the milestones: provisioned, connected, updating
    Debug,      // per-step detail, including payload sizes and HTTP status
};

// Where log lines go. Injectable so a project can send them to the serial port,
// to an SD card, or nowhere.
//
// Called from the main loop and, in async dispatch, possibly from the MQTT
// task. Keep it short and do not block.
using LogFn = void (*)(LogLevel level, const char* tag, const char* message,
                       void* ctx);

const char* toString(LogLevel);

}  // namespace campodata
