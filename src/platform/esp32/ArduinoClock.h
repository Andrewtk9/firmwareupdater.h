#pragma once

#include "campodata/detail/BuildTargets.h"

#if FWUP_TARGET_ARDUINO

#include "campodata/Types.h"

namespace campodata {

// Wall clock for the ping's ts field.
//
// SNTP is started with a zero offset so the C library's time() is UTC. The
// fleet's habit of storing local time and calling it UTC is exactly the bug
// this avoids.
class ArduinoClock {
public:
    void begin();

    // Call periodically; cheap once the clock is valid.
    void tick(uint32_t now_ms);

    uint32_t nowMs() const;

    // False until a plausible time is available. The ping then omits ts rather
    // than inventing one.
    bool utc(int64_t& epoch_s) const;

    // Accepts time from outside the IP stack.
    //
    // SNTP needs an lwIP interface, which a cellular-only device does not have:
    // the modem is reachable only as AT commands. Without this the ts field -
    // required by the ping - would never have a source there.
    //
    // Ignored once a clock is already running, so the modem never drags a device
    // that already has SNTP.
    bool acceptExternalUtc(int64_t epoch_s, ClockSource source);

    ClockSource source() const { return _source; }

private:
    ClockSource _source       = ClockSource::None;
    uint32_t    _last_check_ms = 0;
    bool        _started      = false;
};

}  // namespace campodata

#endif  // FWUP_TARGET_ARDUINO
