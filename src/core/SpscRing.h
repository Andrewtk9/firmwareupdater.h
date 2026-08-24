#pragma once

#include <atomic>
#include <stdio.h>
#include <string.h>

#include "campodata/Types.h"

namespace campodata {

// Single-producer single-consumer ring, used to hand MQTT messages from the
// client's own task to the application loop.
//
// Atomics, not a mutex. On Xtensa these compile to a plain 32-bit load or store
// plus a fence: nothing blocks, nothing can time out, and there is no priority
// inversion. That matters because the producer here is the esp-mqtt task and
// the consumer is whoever calls loop().
template <size_t Slots, size_t SlotBytes>
class SpscRing {
public:
    struct Message {
        char     topic[96]        = {};
        uint8_t  payload[SlotBytes] = {};
        size_t   length           = 0;
    };

    // Producer side. Oversized payloads are dropped rather than truncated: a
    // half message would parse as malformed and hide the real cause.
    bool push(const char* topic, const uint8_t* data, size_t len) {
        if (topic == nullptr || len > SlotBytes) {
            _dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const uint32_t head = _head.load(std::memory_order_relaxed);
        const uint32_t next = (head + 1) % Slots;
        if (next == _tail.load(std::memory_order_acquire)) {
            _dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        Message& slot = _slots[head];
        snprintf(slot.topic, sizeof(slot.topic), "%s", topic);
        if (len > 0 && data != nullptr) memcpy(slot.payload, data, len);
        slot.length = len;

        _head.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side.
    bool pop(Message& out) {
        const uint32_t tail = _tail.load(std::memory_order_relaxed);
        if (tail == _head.load(std::memory_order_acquire)) return false;

        out = _slots[tail];
        _tail.store((tail + 1) % Slots, std::memory_order_release);
        return true;
    }

    // Surfaced in the ping so an undersized ring is visible in the field
    // instead of silently losing orders.
    uint32_t dropped() const { return _dropped.load(std::memory_order_relaxed); }

    void clear() {
        _tail.store(_head.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    Message               _slots[Slots];
    std::atomic<uint32_t> _head{0};
    std::atomic<uint32_t> _tail{0};
    std::atomic<uint32_t> _dropped{0};
};

}  // namespace campodata
