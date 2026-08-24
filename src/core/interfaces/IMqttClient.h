#pragma once

#include "campodata/Types.h"

namespace campodata {

// Everything needed to open a session. All of it arrives in the provisioning
// response, which is why none of it is compiled into the library.
struct MqttSessionConfig {
    const char* host      = nullptr;
    uint16_t    port      = 1883;
    bool        tls       = false;
    bool        verify_ca = true;
    const char* ca_pem    = nullptr;

    const char* username  = nullptr;
    const char* password  = nullptr;
    const char* client_id = nullptr;

    // Spec section 5 recommends a will of {"status":"offline"} on the ping
    // topic, which the table there marks as not retained.
    const char* will_topic   = nullptr;
    const char* will_payload = nullptr;
    uint8_t     will_qos     = 1;
    bool        will_retain  = false;

    uint16_t keepalive_s   = 60;
    uint16_t buffer_bytes  = 2048;
    bool     clean_session = true;
};

// Delivered straight from the client's own task when dispatch is Async.
using MqttDirectCb = void (*)(const char* topic, const uint8_t* payload,
                              size_t len, void* ctx);

class IMqttClient {
public:
    virtual ~IMqttClient() = default;

    virtual bool begin(const MqttSessionConfig& cfg) = 0;
    virtual void end() = 0;

    // Installing a direct callback switches delivery to Async: messages bypass
    // the queue and arrive on the client's task. Passing nullptr goes back to
    // queueing them for poll().
    //
    // The topics the library owns are always queued, so its own state machine
    // keeps a single owner whatever the application picked.
    virtual void setDirectCallback(MqttDirectCb cb, void* ctx) = 0;

    // Topics routed to poll() even in Async mode.
    virtual void reserveTopic(const char* topic) = 0;

    virtual bool connected() const = 0;

    virtual bool subscribe(const char* topic, uint8_t qos) = 0;
    virtual bool publish(const char* topic, const uint8_t* payload, size_t len,
                         uint8_t qos, bool retain) = 0;

    // Drains one inbound message. Returns false when the queue is empty.
    virtual bool poll(char* topic, size_t topic_cap,
                      uint8_t* payload, size_t payload_cap, size_t& len) = 0;

    // Highest QoS this implementation can actually publish. The spec asks for
    // 1 on all three topics; reporting the truth lets the device say so rather
    // than pretend.
    virtual uint8_t maxPublishQos() const = 0;

    virtual uint32_t dropped() const = 0;
};

}  // namespace campodata
