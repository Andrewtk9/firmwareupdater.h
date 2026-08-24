#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "campodata/Types.h"
#include "core/ProvisioningCodec.h"

using namespace campodata;
using namespace campodata::provisioning;

void setUp() {}
void tearDown() {}

// The exact response body from spec section 4.1.
static const char* kSpecResponse = R"({
  "device_id": "9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de",
  "mqtt": {
    "host": "mqtt.example.com",
    "port": 8883,
    "tls": true,
    "username": "dev_9f1c2e6a",
    "password": "K7c!x2Qm",
    "topics": {
      "ping": "/ping/9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de",
      "update": "/update/9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de",
      "config": "/config/9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de"
    }
  },
  "api_base_url": "https://updater.example.com"
})";

// ---------------------------------------------------------------- request ---

void test_request_carries_all_four_spec_fields() {
    ProvisionRequest req;
    req.board_id         = "404C9E7446A8";
    req.hardware_model   = "esp32-wroom-n4";
    req.firmware_version = "2.0.0";
    req.repo             = "campotech/pluviometro";

    char body[256] = {};
    TEST_ASSERT_EQUAL(CodecError::Ok, buildRequest(req, body, sizeof(body)));

    TEST_ASSERT_NOT_NULL(strstr(body, "\"board_id\":\"404C9E7446A8\""));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"hardware_model\":\"esp32-wroom-n4\""));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"firmware_version\":\"2.0.0\""));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"repo\":\"campotech/pluviometro\""));
}

// The deployed server picks the broker profile from `rede`: "gprs" gets
// 1883 plain, anything else falls through to wifi at 8883 with TLS. A cellular
// device that omits this is handed credentials it can never use.
void test_request_carries_rede_for_broker_profile() {
    ProvisionRequest req;
    req.board_id         = "404C9E7446A8";
    req.hardware_model   = "esp32-wroom-n4";
    req.firmware_version = "2.0.0";
    req.repo             = "campotech/pluviometro";

    char body[256] = {};

    req.rede = LinkType::Gprs;
    TEST_ASSERT_EQUAL(CodecError::Ok, buildRequest(req, body, sizeof(body)));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"rede\":\"gprs\""));

    req.rede = LinkType::Wifi;
    TEST_ASSERT_EQUAL(CodecError::Ok, buildRequest(req, body, sizeof(body)));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"rede\":\"wifi\""));
}

// Unknown link: let the server apply its own default instead of forcing one.
void test_request_omits_rede_when_link_unknown() {
    ProvisionRequest req;
    req.board_id         = "404C9E7446A8";
    req.firmware_version = "2.0.0";
    req.repo             = "x";

    char body[256] = {};
    TEST_ASSERT_EQUAL(CodecError::Ok, buildRequest(req, body, sizeof(body)));
    TEST_ASSERT_NULL(strstr(body, "rede"));
}

void test_request_rejects_missing_required_fields() {
    char body[256] = {};

    ProvisionRequest no_board;
    no_board.firmware_version = "2.0.0";
    no_board.repo             = "x";
    TEST_ASSERT_EQUAL(CodecError::MissingField, buildRequest(no_board, body, sizeof(body)));

    ProvisionRequest no_repo;
    no_repo.board_id         = "404C9E7446A8";
    no_repo.firmware_version = "2.0.0";
    TEST_ASSERT_EQUAL(CodecError::MissingField, buildRequest(no_repo, body, sizeof(body)));
}

void test_request_rejects_small_buffer() {
    ProvisionRequest req;
    req.board_id         = "404C9E7446A8";
    req.hardware_model   = "esp32-wroom-n4";
    req.firmware_version = "2.0.0";
    req.repo             = "campotech/pluviometro";

    char tiny[16] = {};
    TEST_ASSERT_EQUAL(CodecError::BufferTooSmall, buildRequest(req, tiny, sizeof(tiny)));
}

// --------------------------------------------------------------- response ---

void test_parses_the_spec_response() {
    Provisioning p;
    TEST_ASSERT_EQUAL(CodecError::Ok, parseResponse(kSpecResponse, p));

    TEST_ASSERT_EQUAL_STRING("9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de", p.device_id);
    TEST_ASSERT_EQUAL_STRING("mqtt.example.com", p.mqtt_host);
    TEST_ASSERT_EQUAL_UINT16(8883, p.mqtt_port);
    TEST_ASSERT_TRUE(p.mqtt_tls);
    TEST_ASSERT_EQUAL_STRING("dev_9f1c2e6a", p.mqtt_user);
    TEST_ASSERT_EQUAL_STRING("K7c!x2Qm", p.mqtt_pass);
    TEST_ASSERT_EQUAL_STRING("/ping/9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de", p.topic_ping);
    TEST_ASSERT_EQUAL_STRING("/update/9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de", p.topic_update);
    TEST_ASSERT_EQUAL_STRING("/config/9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de", p.topic_config);
    TEST_ASSERT_EQUAL_STRING("https://updater.example.com", p.api_base_url);
}

// Topics are optional; everything else is not.
void test_response_without_topics_still_parses() {
    const char* json = R"({"device_id":"abc","mqtt":{"host":"h","port":1883,
        "username":"u","password":"p"},"api_base_url":"http://x"})";
    Provisioning p;
    TEST_ASSERT_EQUAL(CodecError::Ok, parseResponse(json, p));
    TEST_ASSERT_EQUAL_STRING("", p.topic_ping);
    TEST_ASSERT_FALSE(p.mqtt_tls);
}

void test_response_rejects_missing_credentials() {
    const char* no_pass = R"({"device_id":"abc","mqtt":{"host":"h","port":1883,
        "username":"u"},"api_base_url":"http://x"})";
    Provisioning p;
    TEST_ASSERT_EQUAL(CodecError::MissingField, parseResponse(no_pass, p));
}

void test_response_rejects_missing_mqtt_object() {
    const char* json = R"({"device_id":"abc","api_base_url":"http://x"})";
    Provisioning p;
    TEST_ASSERT_EQUAL(CodecError::MissingField, parseResponse(json, p));
}

void test_response_rejects_bad_port() {
    const char* json = R"({"device_id":"a","mqtt":{"host":"h","port":0,
        "username":"u","password":"p"},"api_base_url":"http://x"})";
    Provisioning p;
    TEST_ASSERT_EQUAL(CodecError::BadValue, parseResponse(json, p));
}

void test_response_rejects_malformed_json() {
    Provisioning p;
    TEST_ASSERT_EQUAL(CodecError::MalformedJson, parseResponse("{not json", p));
    TEST_ASSERT_EQUAL(CodecError::MalformedJson, parseResponse(nullptr, p));
}

// A truncated password would authenticate nowhere and fail far from the cause,
// so an oversized field must be rejected outright.
void test_response_rejects_oversized_field_instead_of_truncating() {
    char json[400];
    char long_user[80];
    memset(long_user, 'x', sizeof(long_user) - 1);
    long_user[sizeof(long_user) - 1] = '\0';
    snprintf(json, sizeof(json),
             R"({"device_id":"a","mqtt":{"host":"h","port":1883,"username":"%s",
                "password":"p"},"api_base_url":"http://x"})", long_user);

    Provisioning p;
    TEST_ASSERT_EQUAL(CodecError::MissingField, parseResponse(json, p));
}

// ---------------------------------------------------------------- outcome ---

void test_http_status_mapping_matches_spec() {
    TEST_ASSERT_EQUAL(ProvisionOutcome::Granted,      fromHttpStatus(200));
    // The deployed server answers 400 for a missing board_id, where the spec
    // documents 404. Both are mapped.
    TEST_ASSERT_EQUAL(ProvisionOutcome::BadRequest,   fromHttpStatus(400));
    TEST_ASSERT_EQUAL(ProvisionOutcome::NotReleased,  fromHttpStatus(403));
    TEST_ASSERT_EQUAL(ProvisionOutcome::UnknownBoard, fromHttpStatus(404));
    TEST_ASSERT_EQUAL(ProvisionOutcome::Concurrent,   fromHttpStatus(409));
    TEST_ASSERT_EQUAL(ProvisionOutcome::RateLimited,  fromHttpStatus(429));
    TEST_ASSERT_EQUAL(ProvisionOutcome::ServerError,  fromHttpStatus(503));
}

// 400/403/404 need someone to act: backing off hard is correct.
void test_denials_are_not_transient() {
    TEST_ASSERT_FALSE(isTransient(ProvisionOutcome::BadRequest));
    TEST_ASSERT_FALSE(isTransient(ProvisionOutcome::NotReleased));
    TEST_ASSERT_FALSE(isTransient(ProvisionOutcome::UnknownBoard));

    TEST_ASSERT_TRUE(isTransient(ProvisionOutcome::Concurrent));
    TEST_ASSERT_TRUE(isTransient(ProvisionOutcome::RateLimited));
    TEST_ASSERT_TRUE(isTransient(ProvisionOutcome::ServerError));
    TEST_ASSERT_TRUE(isTransient(ProvisionOutcome::TransportError));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_request_carries_all_four_spec_fields);
    RUN_TEST(test_request_carries_rede_for_broker_profile);
    RUN_TEST(test_request_omits_rede_when_link_unknown);
    RUN_TEST(test_request_rejects_missing_required_fields);
    RUN_TEST(test_request_rejects_small_buffer);

    RUN_TEST(test_parses_the_spec_response);
    RUN_TEST(test_response_without_topics_still_parses);
    RUN_TEST(test_response_rejects_missing_credentials);
    RUN_TEST(test_response_rejects_missing_mqtt_object);
    RUN_TEST(test_response_rejects_bad_port);
    RUN_TEST(test_response_rejects_malformed_json);
    RUN_TEST(test_response_rejects_oversized_field_instead_of_truncating);

    RUN_TEST(test_http_status_mapping_matches_spec);
    RUN_TEST(test_denials_are_not_transient);

    return UNITY_END();
}
