#include "platform/esp32/ArduinoClock.h"

#if FWUP_TARGET_ARDUINO

#include <Arduino.h>
#include <time.h>

#include "core/Iso8601.h"

namespace campodata {
namespace {

// Offset zero on purpose: time() then returns UTC, which is what the ping
// field is specified as. Passing a local offset here is how the fleet ended up
// storing local time under a UTC name.
constexpr long kGmtOffset = 0;
constexpr int  kDaylight  = 0;

// Checking every 5 s is plenty; SNTP takes a few seconds after the link is up.
constexpr uint32_t kCheckIntervalMs = 5000;

}  // namespace

void ArduinoClock::begin() {
    if (_started) return;
    configTime(kGmtOffset, kDaylight, "a.st1.ntp.br", "pool.ntp.org", "time.nist.gov");
    _started = true;
}

void ArduinoClock::tick(uint32_t now_ms) {
    if (_source != ClockSource::None) return;
    if (_started && (now_ms - _last_check_ms) < kCheckIntervalMs) return;

    _last_check_ms = now_ms;
    if (!_started) begin();

    const time_t t = time(nullptr);
    if (iso8601::plausible(static_cast<int64_t>(t))) _source = ClockSource::Sntp;
}

uint32_t ArduinoClock::nowMs() const {
    return millis();
}

bool ArduinoClock::utc(int64_t& epoch_s) const {
    const time_t t = time(nullptr);
    if (!iso8601::plausible(static_cast<int64_t>(t))) return false;
    epoch_s = static_cast<int64_t>(t);
    return true;
}

}  // namespace campodata

#endif  // FWUP_TARGET_ARDUINO
