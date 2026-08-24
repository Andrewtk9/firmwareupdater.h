#pragma once

#include "campodata/LogSink.h"

namespace campodata {
namespace log {

// One sink per firmware. A global rather than a member because logging has to
// work from static helpers and from the MQTT task, and threading a pointer
// through every call site would bury the code it is meant to explain.
void configure(LogFn fn, void* ctx, LogLevel level);

LogLevel level();

// Runtime control. Logging starts enabled at Info; disabling keeps the chosen
// level so a later enable restores it instead of guessing.
void setLevel(LogLevel level);
void setEnabled(bool enabled);
bool enabled();

// printf-style. Formats into a fixed buffer: no allocation on the log path,
// which matters because some of these fire during a flash write.
void write(LogLevel level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

}  // namespace log
}  // namespace campodata

// Compiled out entirely when the level is below the floor, so a release build
// pays nothing for Debug lines.
#ifndef FWUP_LOG_FLOOR
#define FWUP_LOG_FLOOR 4  // Debug
#endif

#define FWUP_LOGE(tag, ...) \
    ::campodata::log::write(::campodata::LogLevel::Error, tag, __VA_ARGS__)
#define FWUP_LOGW(tag, ...) \
    ::campodata::log::write(::campodata::LogLevel::Warn, tag, __VA_ARGS__)
#define FWUP_LOGI(tag, ...) \
    ::campodata::log::write(::campodata::LogLevel::Info, tag, __VA_ARGS__)

#if FWUP_LOG_FLOOR >= 4
#define FWUP_LOGD(tag, ...) \
    ::campodata::log::write(::campodata::LogLevel::Debug, tag, __VA_ARGS__)
#else
#define FWUP_LOGD(tag, ...) do {} while (0)
#endif
