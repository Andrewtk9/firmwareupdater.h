#pragma once

#include "campodata/Types.h"

namespace campodata {

// Persistent storage, spec section 10.
//
// Kept behind an interface so the state machine can be driven by a fake in host
// tests. Confidentiality is not this layer's job: on ESP32 it comes from NVS
// encryption plus flash encryption, whose key lives in eFuse. Encrypting here,
// with a key that ships inside the same binary, would only look secure.
class INvs {
public:
    virtual ~INvs() = default;

    virtual bool getString(const char* key, char* out, size_t cap) const = 0;
    virtual bool setString(const char* key, const char* value) = 0;

    virtual bool getU32(const char* key, uint32_t& out) const = 0;
    virtual bool setU32(const char* key, uint32_t value) = 0;

    virtual bool getU16(const char* key, uint16_t& out) const = 0;
    virtual bool setU16(const char* key, uint16_t value) = 0;

    virtual bool getU8(const char* key, uint8_t& out) const = 0;
    virtual bool setU8(const char* key, uint8_t value) = 0;

    virtual bool erase(const char* key) = 0;
    virtual bool eraseAll() = 0;
    virtual bool commit() = 0;

    virtual bool has(const char* key) const = 0;

    // True when the backing store is actually encrypted at rest, so the device
    // can report honestly instead of the fleet assuming it.
    virtual bool encrypted() const = 0;
};

// NVS key names. ESP-IDF caps these at 15 characters, which is why the longer
// ones are abbreviated.
namespace nvskey {

// Spec section 10, namespace "updater". Names kept verbatim so any server-side
// tooling can read a dump without translation.
inline constexpr const char* kDeviceId      = "device_id";
inline constexpr const char* kMqttUser      = "mqtt_user";
inline constexpr const char* kMqttPass      = "mqtt_pass";
inline constexpr const char* kMqttHost      = "mqtt_host";
inline constexpr const char* kMqttPort      = "mqtt_port";
inline constexpr const char* kApiBaseUrl    = "api_base_url";
inline constexpr const char* kFwVersion     = "fw_version";
inline constexpr const char* kFwRepo        = "fw_repo";
inline constexpr const char* kConfigVersion = "config_version";
inline constexpr const char* kProvisioned   = "provisioned";

// Added by v2.

// Where to POST the first provisioning request. Seeded in production and
// erased the moment provisioning succeeds: after that the device has no reason
// to know the address, and a field unit that never knew it cannot leak it.
inline constexpr const char* kBootstrapUrl  = "boot_url";

// Topic strings as delivered by the server, which is the authority on them.
inline constexpr const char* kTopicPing     = "topic_ping";
inline constexpr const char* kTopicUpdate   = "topic_update";
inline constexpr const char* kTopicConfig   = "topic_config";

// Survives the reboot between applying an image and confirming it.
inline constexpr const char* kPendUpdateId  = "pend_upd_id";
inline constexpr const char* kPendVersion   = "pend_ver";
inline constexpr const char* kPendRepo      = "pend_repo";
inline constexpr const char* kPendPrevVer   = "pend_prev";
inline constexpr const char* kPendStartedAt = "pend_t0";
inline constexpr const char* kPendDeadline  = "pend_deadl";

// Retained topics replay on every reconnect, so the last handled order has to
// be remembered or a link flap re-runs the same update forever.
inline constexpr const char* kLastUpdateId  = "last_upd_id";

inline constexpr const char* kOtaState      = "ota_state";
inline constexpr const char* kAbortReason   = "abort_reason";

// Whether an attached RTC holds UTC or the local time the fleet used to store.
inline constexpr const char* kClockBasis    = "clock_basis";

// Set when the rollback guard finds the window was closed before we ran.
inline constexpr const char* kGuardBypassed = "guard_fail";

inline constexpr const char* kNamespace     = "updater";

}  // namespace nvskey
}  // namespace campodata
