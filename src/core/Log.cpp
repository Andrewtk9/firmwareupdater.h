#include "core/Log.h"

#include <stdarg.h>
#include <stdio.h>

#include "campodata/detail/BuildTargets.h"

#if FWUP_TARGET_ARDUINO
#include <Arduino.h>
#endif

namespace campodata {
namespace log {
namespace {

LogFn    g_fn    = nullptr;
void*    g_ctx   = nullptr;
LogLevel g_level    = LogLevel::Info;
LogLevel g_restore  = LogLevel::Info;

// Long enough for a URL plus context, short enough to sit on the stack during
// an OTA write.
constexpr size_t kLineBytes = 200;

#if FWUP_TARGET_ARDUINO
// Default sink. Tagged and levelled so the fleet's existing serial output
// stays readable next to it.
void serialSink(LogLevel level, const char* tag, const char* message, void*) {
    Serial.printf("[%s/%s] %s\n", toString(level), tag, message);
}
#endif

}  // namespace

void configure(LogFn fn, void* ctx, LogLevel level) {
    g_ctx   = ctx;
    g_level = level;
#if FWUP_TARGET_ARDUINO
    g_fn = (fn != nullptr) ? fn : serialSink;
#else
    g_fn = fn;
#endif
}

LogLevel level() {
    return g_level;
}

void setLevel(LogLevel lvl) {
    g_level = lvl;
    if (lvl != LogLevel::None) g_restore = lvl;
}

void setEnabled(bool on) {
    g_level = on ? g_restore : LogLevel::None;
}

bool enabled() {
    return g_level != LogLevel::None;
}

void write(LogLevel lvl, const char* tag, const char* fmt, ...) {
    if (g_fn == nullptr || lvl > g_level || g_level == LogLevel::None) return;

    char line[kLineBytes];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    g_fn(lvl, tag, line, g_ctx);
}

}  // namespace log

const char* toString(LogLevel l) {
    switch (l) {
        case LogLevel::None:  return "none";
        case LogLevel::Error: return "E";
        case LogLevel::Warn:  return "W";
        case LogLevel::Info:  return "I";
        case LogLevel::Debug: return "D";
    }
    return "?";
}

}  // namespace campodata
