#pragma once

#include <stdint.h>
#include <stddef.h>

namespace campodata {
namespace hex {

// Decodes exactly `bytes * 2` hex chars. Returns false on any non-hex input or
// a length mismatch, leaving `out` untouched.
bool decode(const char* text, uint8_t* out, size_t bytes);

// Writes `bytes * 2` lowercase chars plus a NUL. `cap` must hold both.
bool encode(const uint8_t* data, size_t bytes, char* out, size_t cap);

// Length-checked, non-short-circuiting compare. Not for secrecy, for habit.
bool equal(const uint8_t* a, const uint8_t* b, size_t bytes);

}  // namespace hex
}  // namespace campodata
