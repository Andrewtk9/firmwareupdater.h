#include "core/Hex.h"

namespace campodata {
namespace hex {
namespace {

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool decode(const char* text, uint8_t* out, size_t bytes) {
    if (!text || !out) return false;

    for (size_t i = 0; i < bytes; ++i) {
        const int hi = nibble(text[i * 2]);
        if (hi < 0) return false;
        const int lo = nibble(text[i * 2 + 1]);
        if (lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }

    // Reject trailing junk: the caller asked for an exact-length digest.
    return text[bytes * 2] == '\0';
}

bool encode(const uint8_t* data, size_t bytes, char* out, size_t cap) {
    static const char kDigits[] = "0123456789abcdef";
    if (!data || !out || cap < bytes * 2 + 1) return false;

    for (size_t i = 0; i < bytes; ++i) {
        out[i * 2]     = kDigits[data[i] >> 4];
        out[i * 2 + 1] = kDigits[data[i] & 0x0F];
    }
    out[bytes * 2] = '\0';
    return true;
}

bool equal(const uint8_t* a, const uint8_t* b, size_t bytes) {
    if (!a || !b) return false;

    uint8_t diff = 0;
    for (size_t i = 0; i < bytes; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace hex
}  // namespace campodata
