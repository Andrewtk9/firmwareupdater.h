#include "core/Iso8601.h"

#include <stdio.h>

namespace campodata {
namespace iso8601 {
namespace {

// Howard Hinnant's civil-from-days / days-from-civil, shifted to a March-based
// year so leap days land at the end of the cycle.
int64_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
    const uint32_t doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int32_t& y, uint32_t& m, uint32_t& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int64_t yr = static_cast<int64_t>(yoe) + era * 400;
    const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const uint32_t mp = (5u * doy + 2u) / 153u;
    d = doy - (153u * mp + 2u) / 5u + 1u;
    m = mp + (mp < 10 ? 3 : -9);
    y = static_cast<int32_t>(yr + (m <= 2));
}

bool digits(const char* p, int count, int32_t& out) {
    int32_t v = 0;
    for (int i = 0; i < count; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        v = v * 10 + (p[i] - '0');
    }
    out = v;
    return true;
}

}  // namespace

int64_t toEpoch(const Civil& utc) {
    const int64_t days = daysFromCivil(utc.year, utc.month, utc.day);
    return days * 86400 + utc.hour * 3600 + utc.minute * 60 + utc.second;
}

bool fromEpoch(int64_t epoch_s, Civil& out) {
    if (epoch_s < 0) return false;

    const int64_t days = epoch_s / 86400;
    int64_t rem = epoch_s % 86400;

    int32_t y = 0;
    uint32_t m = 0, d = 0;
    civilFromDays(days, y, m, d);

    out.year   = y;
    out.month  = static_cast<uint8_t>(m);
    out.day    = static_cast<uint8_t>(d);
    out.hour   = static_cast<uint8_t>(rem / 3600);
    rem %= 3600;
    out.minute = static_cast<uint8_t>(rem / 60);
    out.second = static_cast<uint8_t>(rem % 60);
    return true;
}

bool format(int64_t epoch_s, char* out, size_t cap) {
    if (!out || cap < kBufferLen) return false;

    Civil c;
    if (!fromEpoch(epoch_s, c)) return false;

    const int n = snprintf(out, cap, "%04d-%02u-%02uT%02u:%02u:%02uZ",
                           static_cast<int>(c.year), c.month, c.day,
                           c.hour, c.minute, c.second);
    return n > 0 && static_cast<size_t>(n) < cap;
}

bool parse(const char* text, int64_t& epoch_s) {
    if (!text) return false;

    // "YYYY-MM-DD" + separator + "HH:MM:SS" is 19 chars before any offset.
    int32_t year = 0, mon = 0, day = 0, hh = 0, mm = 0, ss = 0;
    if (!digits(text, 4, year) || text[4] != '-') return false;
    if (!digits(text + 5, 2, mon) || text[7] != '-') return false;
    if (!digits(text + 8, 2, day)) return false;

    const char sep = text[10];
    if (sep != 'T' && sep != 't' && sep != ' ') return false;

    if (!digits(text + 11, 2, hh) || text[13] != ':') return false;
    if (!digits(text + 14, 2, mm) || text[16] != ':') return false;
    if (!digits(text + 17, 2, ss)) return false;

    if (mon < 1 || mon > 12 || day < 1 || day > 31) return false;
    if (hh > 23 || mm > 59 || ss > 60) return false;

    Civil c;
    c.year   = year;
    c.month  = static_cast<uint8_t>(mon);
    c.day    = static_cast<uint8_t>(day);
    c.hour   = static_cast<uint8_t>(hh);
    c.minute = static_cast<uint8_t>(mm);
    c.second = static_cast<uint8_t>(ss == 60 ? 59 : ss);

    int64_t value = toEpoch(c);

    const char tail = text[19];
    if (tail == '+' || tail == '-') {
        int32_t oh = 0, om = 0;
        if (!digits(text + 20, 2, oh)) return false;
        const char* min_at = (text[22] == ':') ? text + 23 : text + 22;
        if (!digits(min_at, 2, om)) return false;
        const int64_t offset = oh * 3600 + om * 60;
        value += (tail == '+') ? -offset : offset;
    } else if (tail != '\0' && tail != 'Z' && tail != 'z') {
        return false;
    }

    epoch_s = value;
    return true;
}

int64_t fromGsmLocal(const Civil& local, int8_t tz_quarter_hours) {
    return toEpoch(local) - static_cast<int64_t>(tz_quarter_hours) * 15 * 60;
}

bool plausible(int64_t epoch_s) {
    // 2024-01-01 .. 2100-01-01. Anything below is an unsynchronised modem or a
    // dead RTC backup cell.
    return epoch_s >= 1704067200LL && epoch_s < 4102444800LL;
}

}  // namespace iso8601
}  // namespace campodata
