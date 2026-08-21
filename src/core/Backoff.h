#pragma once

#include <stdint.h>
#include <stddef.h>

namespace campodata {

// Non-blocking escalating retry. Never sleeps: the caller asks whether the
// deadline has passed and gets on with its loop otherwise.
//
// Jitter matters at fleet scale. A site full of devices that all lost the same
// AP will otherwise retry in lockstep and keep knocking the AP over.
class Backoff {
public:
    static constexpr size_t kMaxSteps = 6;

    Backoff() = default;

    // `steps` is borrowed, not copied: pass a static table.
    void configure(const uint32_t* steps, size_t count, uint8_t jitter_percent = 20);

    // True once the current step's delay has elapsed since the last failure.
    bool ready(uint32_t now_ms) const;

    // Records a failure and advances to the next (longer) step.
    void fail(uint32_t now_ms);

    // Records success and returns to the first step.
    void reset();

    uint8_t  attempts() const { return _attempts; }
    uint32_t currentDelayMs() const;

private:
    const uint32_t* _steps   = nullptr;
    size_t          _count   = 0;
    uint8_t         _jitter  = 20;
    uint8_t         _attempts = 0;
    uint32_t        _last_fail_ms = 0;
    uint32_t        _current_delay = 0;
    bool            _armed   = false;
};

// Shared schedules. Values come from what actually works on these links, not
// from round numbers.
namespace backoff {

// Network bring-up: fast early retries, because most failures are transient.
inline constexpr uint32_t kNetwork[] = {2000, 4000, 8000, 12000, 30000};
inline constexpr size_t   kNetworkCount = 5;

// MQTT connect: slower, a failure here usually means the link is unhealthy.
inline constexpr uint32_t kMqtt[] = {3000, 8000, 15000};
inline constexpr size_t   kMqttCount = 3;

// OTA chunk retry inside one download attempt.
inline constexpr uint32_t kDownload[] = {5000, 15000, 45000};
inline constexpr size_t   kDownloadCount = 3;

// Confirm POST. Persisted across reboots: the rollback deadline is running.
inline constexpr uint32_t kConfirm[] = {2000, 5000, 15000, 30000, 60000};
inline constexpr size_t   kConfirmCount = 5;

// Provisioning 403/404 means the board is not released yet. Backing off hard is
// correct: hammering will not change the server's mind.
inline constexpr uint32_t kProvisionDenied[] = {30000, 60000, 120000, 300000};
inline constexpr size_t   kProvisionDeniedCount = 4;

}  // namespace backoff
}  // namespace campodata
