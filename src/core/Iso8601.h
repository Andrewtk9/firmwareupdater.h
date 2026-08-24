#pragma once

#include <stdint.h>
#include <stddef.h>

namespace campodata {
namespace iso8601 {

struct Civil {
    int32_t year   = 1970;
    uint8_t month  = 1;
    uint8_t day    = 1;
    uint8_t hour   = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
};

// Needs 21 bytes: "YYYY-MM-DDTHH:MM:SSZ" plus NUL.
inline constexpr size_t kBufferLen = 21;

// Civil UTC <-> epoch seconds. Proleptic Gregorian, no leap seconds, valid for
// years 1970..9999.
int64_t toEpoch(const Civil& utc);
bool    fromEpoch(int64_t epoch_s, Civil& out);

// Formats epoch seconds as "YYYY-MM-DDTHH:MM:SSZ".
bool format(int64_t epoch_s, char* out, size_t cap);

// Parses "YYYY-MM-DDTHH:MM:SSZ" and the "YYYY-MM-DD HH:MM:SS" the current fleet
// emits. A trailing "Z", "+HH:MM" or "-HH:MM" is honoured; a bare timestamp is
// treated as UTC.
bool parse(const char* text, int64_t& epoch_s);

// Converts a modem's local AT+CCLK? reading to UTC.
//
// TinyGSM reports the timezone in quarter-hours, which is what makes this easy
// to get wrong: pluviometro reads the offset and then discards it, so its RTC
// holds local time while the field is documented as UTC.
int64_t fromGsmLocal(const Civil& local, int8_t tz_quarter_hours);

// Rejects the SIM800L's unsynchronised 1980-01-06 default and other nonsense.
bool plausible(int64_t epoch_s);

}  // namespace iso8601
}  // namespace campodata
