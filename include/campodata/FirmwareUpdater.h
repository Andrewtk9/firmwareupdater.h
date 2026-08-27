#pragma once

#include "campodata/Config.h"
#include "campodata/LogSink.h"
#include "campodata/Types.h"
#include "campodata/detail/BuildTargets.h"

#if FWUP_TARGET_ARDUINO
#include <Arduino.h>
#endif

namespace campodata {

// Why the last publish or subscribe did not reach the broker.
enum class MqttStatus : uint8_t {
    Ok,
    NotConnected,
    NotProvisioned,
    TopicRefused,     // outside what the provisioned ACL allows
    PayloadTooLarge,  // bigger than the configured buffer
    QosDowngraded,    // asked for more than this transport can publish
    Failed,
};

// Session lifecycle notifications.
using MqttEventCb = void (*)(void* ctx);

// Callbacks are function pointer plus context, never std::function: no heap, no
// hidden allocation, and they stay usable from an ISR-adjacent context.

// Add project fields to the ping. `json_object` is an ArduinoJson JsonObject*.
using PingExtendCb = void (*)(void* json_object, void* ctx);

// Called when a remote configuration arrives and has been applied.
//
// `payload` is the document exactly as it came from /config, so a project can
// read the fields that are its own. The specification fixes only a handful of
// keys, and every project in the fleet configures things the spec never mentions
// - baud rate, UDP target, reading window. Without the raw document those
// projects would lose remote configuration on the way to this library.
using RemoteConfigCb = void (*)(const RemoteConfig& cfg, const char* payload, void* ctx);

// Every OTA state transition. Use it to pause sensors, drive a LED, or stop
// whatever must not run during a flash write.
using OtaStateCb = void (*)(OtaState from, OtaState to, void* ctx);

// Return false to refuse an order the library would otherwise accept, for
// instance on a flat battery.
using OrderVetoCb = bool (*)(const UpdateOrder& order, void* ctx);

// Application MQTT messages, for topics the project subscribed to itself.
using MqttMessageCb = void (*)(const char* topic, const uint8_t* payload,
                               size_t len, void* ctx);

// Provisioning, heartbeat, remote configuration and OTA, plus the MQTT session
// the whole device shares.
//
// The session belongs here rather than to the application because the
// credentials do: they arrive in the provisioning response and live in NVS.
// Running a second client alongside would mean two sessions authenticating as
// the same device, which the broker resolves by dropping one of them.
class FirmwareUpdater {
public:
    FirmwareUpdater();
    ~FirmwareUpdater();

    FirmwareUpdater(const FirmwareUpdater&) = delete;
    FirmwareUpdater& operator=(const FirmwareUpdater&) = delete;

    // Validates the configuration, opens NVS, detects the board and checks that
    // the rollback window survived the Arduino core. Returns a typed error so
    // the project can decide between halting and degrading.
    ConfigError begin(const Config& cfg);

    // Non-blocking. Call it every iteration: it services the MQTT keepalive,
    // link bring-up and the OTA download, each within a bounded time slice.
    void loop();

    // ------------------------------------------------------------- status --

    bool         isProvisioned() const;
    DeviceState  state() const;
    OtaState     otaState() const;
    Capabilities capabilities() const;
    bool         deviceId(char* out, size_t cap) const;

    // True while an update is downloading, verifying or being applied. Deep
    // sleep must be inhibited whenever this is set.
    bool isBusy() const;

    // Why sleep is blocked, so a battery-powered project can log the reason
    // instead of guessing.
    SleepBlock sleepBlock() const;
    bool       canSleep() const;

    // -------------------------------------------------------------- mqtt ---
    //
    // The full client surface, so a project migrating from PubSubClient or
    // AsyncMqttClient finds the call it already knows. Topic, QoS and retain
    // are the caller's choice on every publish and subscribe.

    bool mqttConnected() const;

    // Last broker-side result, for logging a refused publish or subscribe.
    MqttStatus  mqttStatus() const;
    const char* clientId() const;

    // QoS defaults to 1, which is what the spec asks for on its own topics.
    // A request above capabilities().mqtt_max_publish_qos is downgraded rather
    // than refused, and mqttStatus() reports it.
    bool publish(const char* topic, const char* payload,
                 uint8_t qos = 1, bool retain = false);
    bool publish(const char* topic, const uint8_t* payload, size_t len,
                 uint8_t qos = 1, bool retain = false);

#if FWUP_TARGET_ARDUINO
    // Convenience for the common case: a JSON document already serialised into
    // a String, which is how every project in the fleet publishes today.
    bool publish(const char* topic, const String& payload,
                 uint8_t qos = 1, bool retain = false);
    bool subscribe(const String& topic, uint8_t qos = 1);
#endif

    // Subscriptions are remembered and reapplied on every reconnect, so a
    // project registers them once in setup() and forgets about them. Wildcards
    // are passed through to the broker untouched.
    bool subscribe(const char* topic, uint8_t qos = 1);
    bool unsubscribe(const char* topic);

    // Everything the project subscribed to arrives here. The library's own
    // three topics are handled internally and never reach this callback.
    void onMessage(MqttMessageCb cb, void* ctx);

    // Session lifecycle, for projects that need to know: flush a backlog on
    // connect, start writing to the SD card on disconnect.
    void onMqttConnect(MqttEventCb cb, void* ctx);
    void onMqttDisconnect(MqttEventCb cb, void* ctx);

    // Builds an application topic in the project's own namespace, isolated by
    // device_id - the same pair provisioning grants in the ACL:
    //
    //     medidor-potencia-ac/<device_id>/ret
    //
    // The project name is the last segment of `repo`. Empty until the device is
    // provisioned, since the device_id comes from the server.
    bool appTopic(const char* suffix, char* out, size_t cap) const;

    // ---------------------------------------------------------- callbacks --

    void onPingExtend(PingExtendCb cb, void* ctx);
    void onRemoteConfig(RemoteConfigCb cb, void* ctx);
    void onOtaState(OtaStateCb cb, void* ctx);
    void onOrderVeto(OrderVetoCb cb, void* ctx);

    // ----------------------------------------------------------- logging ---
    //
    // On by default at Info, which narrates the cold start: board, rollback
    // window, provisioning result, broker, subscriptions. Debug adds HTTP
    // status, payload sizes and per-publish detail.

    void setLogging(bool enabled);
    void setLogLevel(LogLevel level);
    bool logging() const;

    // -------------------------------------------------------- production ---

    // Writes the provisioning URL to NVS. Called once per unit before it ships;
    // the library erases it as soon as provisioning succeeds, so a field device
    // no longer carries the address.
    bool seedBootstrapUrl(const char* url);

    // Wipes NVS so the unit provisions again. The server still has to unlock
    // the board_id, which stays blocked after its single delivery.
    bool factoryReset();

    // Opaque, defined in the .cpp. Public only so the implementation's own
    // helpers can name it; nothing outside the library should touch it.
    struct Impl;

private:
    Impl* _impl;
};

}  // namespace campodata
