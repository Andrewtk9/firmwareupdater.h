#include <unity.h>

#include <ArduinoJson.h>
#include <string.h>

#include "campodata/Config.h"
#include "campodata/Types.h"
#include "core/PingBuilder.h"
#include "core/TopicResolver.h"

using namespace campodata;

void setUp() {}
void tearDown() {}

static PingSnapshot makeSnapshot() {
    PingSnapshot s;
    s.ts               = 1755698733;  // 2025-08-20T14:05:33Z
    s.ts_source        = ClockSource::Sntp;
    s.firmware_version = "1.4.2";
    s.repo             = "campotech/sensor-firmware";
    s.uptime_s         = 84213;
    s.link             = LinkType::Wifi;
    s.rssi             = -61;
    s.free_heap        = 142336;
    s.ota_state        = OtaState::Idle;
    s.config_version   = 7;
    return s;
}

// ------------------------------------------------------------------- ping ---

// Spec section 6, field for field.
void test_ping_has_every_spec_field() {
    PingBuilder pb;
    char out[768] = {};
    TEST_ASSERT_EQUAL(CodecError::Ok, pb.build(makeSnapshot(), false, out, sizeof(out)));

    JsonDocument doc;
    TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, out).code());

    TEST_ASSERT_EQUAL_STRING("2025-08-20T14:05:33Z", doc["ts"]);
    TEST_ASSERT_EQUAL_STRING("1.4.2", doc["firmware_version"]);
    TEST_ASSERT_EQUAL_STRING("campotech/sensor-firmware", doc["repo"]);
    TEST_ASSERT_EQUAL_UINT32(84213, doc["uptime_s"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("wifi", doc["link"]);
    TEST_ASSERT_EQUAL_INT(-61, doc["rssi"].as<int>());
    TEST_ASSERT_EQUAL_UINT32(142336, doc["free_heap"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("idle", doc["ota_state"]);
    TEST_ASSERT_EQUAL_UINT32(7, doc["config_version"].as<uint32_t>());
}

// A single-link device must look exactly like the spec to an untouched server.
void test_single_link_ping_carries_no_extensions() {
    PingBuilder pb;
    char out[768] = {};
    pb.build(makeSnapshot(), false, out, sizeof(out));

    TEST_ASSERT_NULL(strstr(out, "links"));
    TEST_ASSERT_NULL(strstr(out, "ota_link_ready"));
}

void test_dual_link_ping_adds_link_detail() {
    PingSnapshot s = makeSnapshot();
    s.link                 = LinkType::Gprs;  // MQTT rides cellular...
    s.wifi_up              = true;            // ...but Wi-Fi is up for OTA
    s.wifi_rssi            = -58;
    s.gprs_up              = true;
    s.gprs_rssi            = -71;
    s.ota_link_ready       = true;
    s.ota_transport_secure = true;

    PingBuilder pb;
    char out[768] = {};
    TEST_ASSERT_EQUAL(CodecError::Ok, pb.build(s, true, out, sizeof(out)));

    JsonDocument doc;
    deserializeJson(doc, out);

    // The spec field keeps its spec meaning: where MQTT actually is.
    TEST_ASSERT_EQUAL_STRING("gprs", doc["link"]);
    TEST_ASSERT_TRUE(doc["links"]["wifi"]["up"].as<bool>());
    TEST_ASSERT_EQUAL_INT(-58, doc["links"]["wifi"]["rssi"].as<int>());
    TEST_ASSERT_TRUE(doc["ota_link_ready"].as<bool>());
}

// Omitting ts beats inventing one: a fake epoch corrupts the series silently.
void test_ping_omits_ts_when_clock_is_unknown() {
    PingSnapshot s = makeSnapshot();
    s.ts        = 0;
    s.ts_source = ClockSource::None;

    PingBuilder pb;
    char out[768] = {};
    pb.build(s, false, out, sizeof(out));

    JsonDocument doc;
    deserializeJson(doc, out);
    TEST_ASSERT_TRUE(doc["ts"].isNull());
    TEST_ASSERT_TRUE(doc["ts_source"].isNull());
    TEST_ASSERT_EQUAL_UINT32(84213, doc["uptime_s"].as<uint32_t>());
}

// The SIM800L's unsynchronised default must not reach the server.
void test_ping_omits_implausible_clock() {
    PingSnapshot s = makeSnapshot();
    s.ts = 315532800;  // 1980
    PingBuilder pb;
    char out[768] = {};
    pb.build(s, false, out, sizeof(out));

    JsonDocument doc;
    deserializeJson(doc, out);
    TEST_ASSERT_TRUE(doc["ts"].isNull());
}

void test_ping_carries_abort_reason() {
    PingSnapshot s = makeSnapshot();
    s.ota_state    = OtaState::Aborted;
    s.abort_reason = AbortReason::Gprs;

    PingBuilder pb;
    char out[768] = {};
    pb.build(s, false, out, sizeof(out));

    JsonDocument doc;
    deserializeJson(doc, out);
    TEST_ASSERT_EQUAL_STRING("aborted", doc["ota_state"]);
    TEST_ASSERT_EQUAL_STRING("gprs", doc["reason"]);
}

static void addProjectFields(void* obj, void*) {
    JsonObject& root = *static_cast<JsonObject*>(obj);
    root["contador"] = 42;
    root["bat_mv"]   = 3940;
}

// Every project needs its own fields; the library must not know about them.
void test_project_can_extend_the_ping() {
    PingBuilder pb;
    pb.setExtender(addProjectFields, nullptr);

    char out[768] = {};
    pb.build(makeSnapshot(), false, out, sizeof(out));

    JsonDocument doc;
    deserializeJson(doc, out);
    TEST_ASSERT_EQUAL_INT(42, doc["contador"].as<int>());
    TEST_ASSERT_EQUAL_INT(3940, doc["bat_mv"].as<int>());
    TEST_ASSERT_EQUAL_STRING("1.4.2", doc["firmware_version"]);
}

void test_ping_rejects_small_buffer() {
    PingBuilder pb;
    char tiny[32] = {};
    TEST_ASSERT_EQUAL(CodecError::BufferTooSmall,
                      pb.build(makeSnapshot(), false, tiny, sizeof(tiny)));
}

void test_will_payload_matches_spec() {
    TEST_ASSERT_EQUAL_STRING("{\"status\":\"offline\"}", PingBuilder::willPayload());
    // Spec section 5 marks /ping as retain = no, so the will follows it.
    TEST_ASSERT_FALSE(PingBuilder::kWillRetain);
}

// ------------------------------------------------------------------ topics --

void test_topics_use_the_spec_format() {
    TopicConfig cfg;  // defaults to MqttTopicScheme::Pdf
    TopicResolver r;
    TEST_ASSERT_TRUE(r.build(cfg, "9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de"));

    TEST_ASSERT_EQUAL_STRING("/ping/9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de", r.ping());
    TEST_ASSERT_EQUAL_STRING("/update/9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de", r.update());
    TEST_ASSERT_EQUAL_STRING("/config/9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de", r.config());
}

void test_topics_need_a_device_id() {
    TopicConfig cfg;
    TopicResolver r;
    TEST_ASSERT_FALSE(r.build(cfg, ""));
    TEST_ASSERT_FALSE(r.ready());
}

// The server is the authority: whatever provisioning returned wins.
void test_provisioned_topics_override_defaults() {
    TopicConfig cfg;
    TopicResolver r;
    r.build(cfg, "abc");

    Provisioning p;
    snprintf(p.topic_ping, sizeof(p.topic_ping), "%s", "updater/pluviometro/ping/abc");
    snprintf(p.topic_update, sizeof(p.topic_update), "%s", "updater/pluviometro/update/abc");
    r.applyProvisioned(p);

    TEST_ASSERT_EQUAL_STRING("updater/pluviometro/ping/abc", r.ping());
    TEST_ASSERT_EQUAL_STRING("updater/pluviometro/update/abc", r.update());
    // Not supplied, so the computed default survives.
    TEST_ASSERT_EQUAL_STRING("/config/abc", r.config());
}

// With no per-device ACL on the broker, this is the only containment there is.
void test_device_may_only_publish_to_its_own_ping() {
    TopicConfig cfg;
    TopicResolver r;
    r.build(cfg, "abc");

    TEST_ASSERT_TRUE(r.mayPublish("/ping/abc"));
    TEST_ASSERT_FALSE(r.mayPublish("/ping/someone-else"));
    TEST_ASSERT_FALSE(r.mayPublish("/update/abc"));
    TEST_ASSERT_FALSE(r.mayPublish(nullptr));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_ping_has_every_spec_field);
    RUN_TEST(test_single_link_ping_carries_no_extensions);
    RUN_TEST(test_dual_link_ping_adds_link_detail);
    RUN_TEST(test_ping_omits_ts_when_clock_is_unknown);
    RUN_TEST(test_ping_omits_implausible_clock);
    RUN_TEST(test_ping_carries_abort_reason);
    RUN_TEST(test_project_can_extend_the_ping);
    RUN_TEST(test_ping_rejects_small_buffer);
    RUN_TEST(test_will_payload_matches_spec);

    RUN_TEST(test_topics_use_the_spec_format);
    RUN_TEST(test_topics_need_a_device_id);
    RUN_TEST(test_provisioned_topics_override_defaults);
    RUN_TEST(test_device_may_only_publish_to_its_own_ping);

    return UNITY_END();
}
