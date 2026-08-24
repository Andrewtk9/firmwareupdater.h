#pragma once

#include "campodata/detail/BuildTargets.h"

#if FWUP_TARGET_ESP32

#include <mqtt_client.h>

#include "core/SpscRing.h"
#include "core/interfaces/IMqttClient.h"

namespace campodata {

// MQTT over esp-mqtt, which ships inside the Arduino framework.
//
// Chosen over PubSubClient because the spec requires QoS 1 on all three topics
// and PubSubClient can only publish QoS 0. It also gives TLS and a proper LWT
// without extra work.
//
// It runs its own task, so the event handler does no parsing: it copies the
// message into a lock-free ring and returns. Everything else happens on the
// caller's loop, keeping a single owner for the state machine.
class EspMqttClient final : public IMqttClient {
public:
    // Sized for this protocol: the largest message is an update order at a few
    // hundred bytes. Six slots of 1 KB is 6 KB of RAM.
    using Ring = SpscRing<6, 1024>;

    EspMqttClient() = default;
    ~EspMqttClient() override;

    EspMqttClient(const EspMqttClient&) = delete;
    EspMqttClient& operator=(const EspMqttClient&) = delete;

    bool begin(const MqttSessionConfig& cfg) override;
    void end() override;

    void setDirectCallback(MqttDirectCb cb, void* ctx) override;
    void reserveTopic(const char* topic) override;

    bool connected() const override { return _connected; }

    bool subscribe(const char* topic, uint8_t qos) override;
    bool publish(const char* topic, const uint8_t* payload, size_t len,
                 uint8_t qos, bool retain) override;

    bool poll(char* topic, size_t topic_cap,
              uint8_t* payload, size_t payload_cap, size_t& len) override;

    uint8_t  maxPublishQos() const override { return 1; }
    uint32_t dropped() const override { return _ring.dropped(); }

private:
    static void onEvent(void* arg, esp_event_base_t base, int32_t id, void* data);
    void handle(esp_mqtt_event_handle_t event);
    void deliver(const char* topic, const uint8_t* payload, size_t len);
    bool isReserved(const char* topic) const;

    static constexpr size_t kMaxReserved = 4;

    esp_mqtt_client_handle_t _client = nullptr;
    Ring                     _ring;

    MqttDirectCb _direct     = nullptr;
    void*        _direct_ctx = nullptr;

    // The library's own topics, always queued so its state machine stays on a
    // single owner even when the project chose async delivery.
    char   _reserved[kMaxReserved][kMaxTopicLen] = {};
    size_t _reserved_count = 0;

    // Written by the esp-mqtt task, read by the application loop. volatile is
    // enough here: a single bool with one writer, and the project rule is no
    // semaphores on ESP32.
    volatile bool _connected = false;
    volatile bool _started   = false;

    // esp-mqtt splits payloads larger than its buffer across events, so a
    // fragmented message is reassembled before it reaches the ring - a
    // half-delivered order must never be visible to the consumer.
    static constexpr size_t kAssemblyBytes = 1024;
    uint8_t _assembly[kAssemblyBytes] = {};
    char    _assembly_topic[96]       = {};
    size_t  _assembly_len             = 0;
    bool    _assembling               = false;
};

}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
