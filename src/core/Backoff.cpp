#include "core/Backoff.h"

namespace campodata {
namespace {

// Entropy comes from the failure timestamp itself. Two devices rarely fail on
// the same millisecond, so they decorrelate without needing an RNG - and the
// result stays deterministic, which keeps it testable.
uint32_t scramble(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

uint32_t applyJitter(uint32_t base_ms, uint8_t percent, uint32_t entropy) {
    if (percent == 0 || base_ms == 0) return base_ms;

    const uint32_t span = (base_ms / 100u) * percent;   // +/- span
    if (span == 0) return base_ms;

    const uint32_t offset = scramble(entropy) % (span * 2u + 1u);
    const int64_t jittered = static_cast<int64_t>(base_ms) + offset - span;
    return jittered < 0 ? 0 : static_cast<uint32_t>(jittered);
}

}  // namespace

void Backoff::configure(const uint32_t* steps, size_t count, uint8_t jitter_percent) {
    _steps   = steps;
    _count   = count;
    _jitter  = jitter_percent;
    reset();
}

void Backoff::reset() {
    _attempts      = 0;
    _last_fail_ms  = 0;
    _current_delay = 0;
    _armed         = false;
}

void Backoff::fail(uint32_t now_ms) {
    if (!_steps || _count == 0) return;

    const size_t idx = (_attempts < _count) ? _attempts : _count - 1;
    _current_delay = applyJitter(_steps[idx], _jitter, now_ms ^ (_attempts * 0x9E3779B9U));
    _last_fail_ms  = now_ms;
    _armed         = true;

    if (_attempts < 0xFF) ++_attempts;
}

bool Backoff::ready(uint32_t now_ms) const {
    if (!_armed) return true;
    // Unsigned subtraction is correct across the 49-day millis() wrap.
    return (now_ms - _last_fail_ms) >= _current_delay;
}

uint32_t Backoff::currentDelayMs() const {
    return _armed ? _current_delay : 0;
}

}  // namespace campodata
