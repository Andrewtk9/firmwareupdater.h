#pragma once

// Core vocabulary types. Must stay free of Arduino.h, esp_* and String so the
// state machine can be unit-tested on the host.

#include <stdint.h>
#include <stddef.h>

namespace campodata {

inline constexpr size_t kSha256Bytes   = 32;
inline constexpr size_t kSha256HexLen  = 64;
inline constexpr size_t kMaxTopicLen   = 128;
inline constexpr size_t kMaxUrlLen     = 256;
inline constexpr size_t kMaxIdLen      = 40;  // UUID v4 with room to spare
inline constexpr size_t kMaxVersionLen = 24;
inline constexpr size_t kMaxRepoLen    = 64;
inline constexpr size_t kMaxModelLen   = 32;

enum class LinkType  : uint8_t { None, Wifi, Gprs };
enum class LinkState : uint8_t { Down, Starting, Up, Degraded, Stopping, Fault };

enum class LinkMode       : uint8_t { Wifi, Gprs, Both };
enum class LinkPreference : uint8_t { WifiPreferred, GprsPreferred };

// The spec pins OTA downloads to Wi-Fi. AnyLink is the documented exception and
// additionally requires allow_ota_on_gprs.
enum class OtaLinkPolicy : uint8_t { WifiOnly, AnyLink };

enum class MqttBackend     : uint8_t { Auto, PubSub, EspMqtt };
enum class MqttTopicScheme : uint8_t { Pdf, SlugEnvRet };

enum class ClockPolicy : uint8_t { RequireUtc, BestEffort };
enum class ClockSource : uint8_t { None, Sntp, Gsm, Rtc, Server };

// Device state machine, spec section 7.4.
enum class DeviceState : uint8_t {
    Boot,
    Provisioning,
    Operation,
    EvaluatingOrder,
    AwaitingButton,
    Downloading,
    Verifying,
    Applying,
    PendingReboot,
    Confirming,
    RollingBack,
    Failed,
};

// What the ping reports. Serialised with the spec's lowercase spellings.
enum class OtaState : uint8_t {
    Idle,
    Aborted,
    PendingUser,
    Downloading,
    Verifying,
    Applying,
    PendingReboot,
    Confirming,
    Failed,
};

enum class AbortReason : uint8_t {
    None,
    Gprs,
    NoWifi,
    Checksum,
    Network,
    Stall,
    Flash,
    NoSpace,
    NoOtaPartition,
    SizeMismatch,
    ServerReject,
    UserWindow,
    Downgrade,
    AppVeto,
};

enum class ConfirmStatus : uint8_t { Success, Failed, RolledBack };

enum class SleepBlock : uint8_t {
    None,
    Provisioning,
    AwaitingButton,
    Downloading,
    Verifying,
    Applying,
    PendingConfirm,
    ClockSyncing,
};

enum class ConfigError : uint8_t {
    Ok,
    MissingFirmwareVersion,
    MissingRepo,
    GprsPinsUnset,
    SlugRequiredForScheme,
    TlsWithoutCaAndNotInsecure,
    OtaOnGprsWithoutOptIn,
    BothModeMissingWifiCreds,
};

enum class OtaSinkError : uint8_t {
    Ok,
    NoPartition,
    TooLarge,
    BeginFailed,
    WriteFailed,
    ChecksumMismatch,
    EndFailed,
    SetBootFailed,
    NotOpen,
    AlreadyOpen,
};

enum class HttpError : uint8_t {
    Ok,
    WouldBlock,
    Eof,
    ConnectFailed,
    BadStatus,
    RangeUnsupported,
    Timeout,
    TooLarge,
    Transport,
};

enum class ProtoError : uint8_t {
    Ok,
    NotReady,
    Transport,
    BadResponse,
    Unauthorized,
    NotFound,
    Conflict,
    RateLimited,
    ServerError,
};

// Shared by every JSON codec. Fields are rejected rather than truncated: a
// silently shortened device_id or MQTT password fails much later and much more
// confusingly than an outright error.
enum class CodecError : uint8_t {
    Ok,
    BufferTooSmall,
    MalformedJson,
    MissingField,
    FieldTooLong,
    BadValue,
};

// An OTA order as delivered on /update/<id>. Fixed storage: the FSM never allocates.
struct UpdateOrder {
    char     update_id[kMaxIdLen]           = {};
    char     target_version[kMaxVersionLen] = {};
    char     repo[kMaxRepoLen]              = {};
    char     url[kMaxUrlLen]                = {};
    uint32_t size_bytes                     = 0;
    uint8_t  sha256[kSha256Bytes]           = {};
    bool     has_sha256                     = false;
    bool     mandatory                      = false;
    bool     requires_button                = true;
    int64_t  issued_at                      = 0;  // epoch seconds, 0 = absent
};

// Remote configuration from /config/<id>. Only the spec's tunables: endpoints
// and credentials are deliberately not settable from here.
struct RemoteConfig {
    uint32_t config_version         = 0;
    uint16_t ping_interval_s        = 60;
    uint16_t ota_button_window_s    = 300;
    bool     allow_ota_on_gprs      = false;
    uint16_t mqtt_keepalive_s       = 60;
    uint32_t sensor_read_interval_s = 0;
    bool     has_sensor_interval    = false;
    bool     applied_at_required    = false;
    int64_t  server_time            = 0;  // extension: 0 = absent
};

struct OtaSlotInfo {
    uint32_t size_bytes = 0;
    uint32_t offset     = 0;
    uint8_t  subtype    = 0;
    bool     valid      = false;
};

struct LinkStats {
    int16_t  rssi        = 0;
    uint32_t up_since_ms = 0;
    uint32_t drops       = 0;
    bool     has_ip      = false;
};

// Everything the ping needs, gathered once per publish.
struct PingSnapshot {
    int64_t     ts               = 0;  // epoch seconds, 0 = unknown
    ClockSource ts_source        = ClockSource::None;
    const char* firmware_version = nullptr;
    const char* repo             = nullptr;
    uint32_t    uptime_s         = 0;
    LinkType    link             = LinkType::None;
    int16_t     rssi             = 0;
    uint32_t    free_heap        = 0;
    OtaState    ota_state        = OtaState::Idle;
    AbortReason abort_reason     = AbortReason::None;
    uint32_t    config_version   = 0;

    // Additive extensions, documented in docs/SPEC-EXTENSIONS.md.
    bool        wifi_up              = false;
    bool        gprs_up              = false;
    int16_t     wifi_rssi            = 0;
    int16_t     gprs_rssi            = 0;
    bool        ota_link_ready       = false;
    bool        ota_transport_secure = false;
    uint8_t     pub_qos              = 0;
    uint32_t    mqtt_dropped         = 0;
    const char* hardware_model       = nullptr;
};

struct ConfirmReport {
    char          update_id[kMaxIdLen]             = {};
    ConfirmStatus status                           = ConfirmStatus::Success;
    char          firmware_version[kMaxVersionLen] = {};
    char          repo[kMaxRepoLen]                = {};
    char          previous_version[kMaxVersionLen] = {};
    uint32_t      duration_ms                      = 0;
    AbortReason   error_code                       = AbortReason::None;
    const char*   error_message                    = nullptr;
};

// Result of the provisioning handshake.
struct Provisioning {
    char     device_id[kMaxIdLen]       = {};
    char     mqtt_host[64]              = {};
    uint16_t mqtt_port                  = 0;
    bool     mqtt_tls                   = false;
    char     mqtt_user[48]              = {};
    char     mqtt_pass[64]              = {};
    char     topic_ping[kMaxTopicLen]   = {};
    char     topic_update[kMaxTopicLen] = {};
    char     topic_config[kMaxTopicLen] = {};
    char     api_base_url[96]           = {};
};

// Runtime truth about what this build, on this link, can actually do. Reported
// in the ping so the server never has to guess.
struct Capabilities {
    bool    ota_capable          = false;
    bool    rollback_supported   = false;
    bool    range_supported      = false;
    bool    sha256_supported     = true;
    uint8_t mqtt_max_publish_qos = 0;
    bool    tls_on_active_link   = false;
    uint8_t tier                 = 1;  // 1 = Arduino 2.x, 2 = Arduino 3.x, 3 = ESP-IDF
};

const char* toString(OtaState);
const char* toString(AbortReason);
const char* toString(ConfirmStatus);
const char* toString(LinkType);
const char* toString(DeviceState);
const char* toString(ConfigError);
const char* toString(OtaSinkError);
const char* toString(HttpError);

}  // namespace campodata
