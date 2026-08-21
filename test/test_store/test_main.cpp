#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "campodata/Types.h"
#include "core/DeviceStore.h"
#include "support/FakeNvs.h"

using namespace campodata;

void setUp() {}
void tearDown() {}

static Provisioning makeProvisioning() {
    Provisioning p;
    snprintf(p.device_id, sizeof(p.device_id), "%s",
             "9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de");
    snprintf(p.mqtt_host, sizeof(p.mqtt_host), "%s", "mqtt.example.com");
    p.mqtt_port = 8883;
    p.mqtt_tls  = true;
    snprintf(p.mqtt_user, sizeof(p.mqtt_user), "%s", "dev_9f1c2e6a");
    snprintf(p.mqtt_pass, sizeof(p.mqtt_pass), "%s", "K7c!x2Qm");
    snprintf(p.topic_ping, sizeof(p.topic_ping), "%s", "/ping/9f1c2e6a");
    snprintf(p.topic_update, sizeof(p.topic_update), "%s", "/update/9f1c2e6a");
    snprintf(p.topic_config, sizeof(p.topic_config), "%s", "/config/9f1c2e6a");
    snprintf(p.api_base_url, sizeof(p.api_base_url), "%s", "https://updater.example.com");
    return p;
}

// ------------------------------------------------- bootstrap url lifecycle --

// The whole point: seed it, use it once, and the device never carries it again.
void test_bootstrap_url_is_forgotten_after_provisioning() {
    FakeNvs nvs;
    DeviceStore store(nvs);
    char url[kMaxUrlLen] = {};

    TEST_ASSERT_TRUE(store.seedBootstrapUrl("https://updater.example.com"));
    TEST_ASSERT_TRUE(store.hasBootstrapUrl());
    TEST_ASSERT_TRUE(store.bootstrapUrl(url, sizeof(url), nullptr));
    TEST_ASSERT_EQUAL_STRING("https://updater.example.com", url);

    TEST_ASSERT_TRUE(store.saveProvisioning(makeProvisioning()));

    // Gone for good.
    TEST_ASSERT_FALSE(store.hasBootstrapUrl());
    TEST_ASSERT_FALSE(store.bootstrapUrl(url, sizeof(url), nullptr));

    // But the device still knows where the API lives, from the response.
    char api[128] = {};
    TEST_ASSERT_TRUE(store.apiBaseUrl(api, sizeof(api)));
    TEST_ASSERT_EQUAL_STRING("https://updater.example.com", api);
}

// A unit that was never seeded falls back to the compiled-in default.
void test_bootstrap_url_falls_back_to_compiled_default() {
    FakeNvs nvs;
    DeviceStore store(nvs);
    char url[kMaxUrlLen] = {};

    TEST_ASSERT_TRUE(store.bootstrapUrl(url, sizeof(url), "https://fallback.example.com"));
    TEST_ASSERT_EQUAL_STRING("https://fallback.example.com", url);
}

// Seeded value wins over the compiled-in one.
void test_seeded_bootstrap_url_beats_default() {
    FakeNvs nvs;
    DeviceStore store(nvs);
    char url[kMaxUrlLen] = {};

    store.seedBootstrapUrl("https://seeded.example.com");
    store.bootstrapUrl(url, sizeof(url), "https://fallback.example.com");
    TEST_ASSERT_EQUAL_STRING("https://seeded.example.com", url);
}

// ------------------------------------------------------------ provisioning --

void test_provisioning_round_trip() {
    FakeNvs nvs;
    DeviceStore store(nvs);

    TEST_ASSERT_FALSE(store.isProvisioned());
    TEST_ASSERT_TRUE(store.saveProvisioning(makeProvisioning()));
    TEST_ASSERT_TRUE(store.isProvisioned());

    char id[kMaxIdLen] = {};
    TEST_ASSERT_TRUE(store.deviceId(id, sizeof(id)));
    TEST_ASSERT_EQUAL_STRING("9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de", id);

    uint16_t port = 0;
    TEST_ASSERT_TRUE(store.mqttPort(port));
    TEST_ASSERT_EQUAL_UINT16(8883, port);

    char ping[kMaxTopicLen] = {}, upd[kMaxTopicLen] = {}, cfg[kMaxTopicLen] = {};
    TEST_ASSERT_TRUE(store.topics(ping, sizeof(ping), upd, sizeof(upd), cfg, sizeof(cfg)));
    TEST_ASSERT_EQUAL_STRING("/ping/9f1c2e6a", ping);
}

void test_provisioning_rejects_incomplete_response() {
    FakeNvs nvs;
    DeviceStore store(nvs);

    Provisioning p = makeProvisioning();
    p.mqtt_pass[0] = '\0';
    TEST_ASSERT_FALSE(store.saveProvisioning(p));
    TEST_ASSERT_FALSE(store.isProvisioned());
}

// A device that thinks it is provisioned but cannot authenticate is worse than
// one that simply retries, so a failed write must not leave the flag set.
void test_failed_write_does_not_mark_provisioned() {
    FakeNvs nvs;
    DeviceStore store(nvs);

    nvs.fail_writes = true;
    TEST_ASSERT_FALSE(store.saveProvisioning(makeProvisioning()));
    nvs.fail_writes = false;
    TEST_ASSERT_FALSE(store.isProvisioned());
}

// The flag alone means nothing without the id the topics are keyed on.
void test_flag_without_device_id_is_not_provisioned() {
    FakeNvs nvs;
    DeviceStore store(nvs);
    nvs.setU8(nvskey::kProvisioned, 1);
    TEST_ASSERT_FALSE(store.isProvisioned());
}

// ---------------------------------------------------------------- versions --

// Spec section 9.2: the version is written only after the confirm returns 200.
void test_firmware_version_commit() {
    FakeNvs nvs;
    DeviceStore store(nvs);

    char v[kMaxVersionLen] = {};
    TEST_ASSERT_FALSE(store.firmwareVersion(v, sizeof(v)));

    TEST_ASSERT_TRUE(store.commitFirmware("1.5.0", "campotech/sensor-firmware"));
    TEST_ASSERT_TRUE(store.firmwareVersion(v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("1.5.0", v);
}

// The config topic is retained and replays on every reconnect, so only a
// strictly newer version may be applied.
void test_config_version_only_moves_forward() {
    FakeNvs nvs;
    DeviceStore store(nvs);

    TEST_ASSERT_EQUAL_UINT32(0, store.configVersion());
    TEST_ASSERT_TRUE(store.applyConfigVersion(7));
    TEST_ASSERT_EQUAL_UINT32(7, store.configVersion());

    TEST_ASSERT_FALSE(store.applyConfigVersion(7));   // replay
    TEST_ASSERT_FALSE(store.applyConfigVersion(3));   // stale
    TEST_ASSERT_EQUAL_UINT32(7, store.configVersion());

    TEST_ASSERT_TRUE(store.applyConfigVersion(8));
}

// ----------------------------------------------------------------- pending --

// This is what survives the reboot between applying an image and confirming it.
void test_pending_update_survives_reboot() {
    FakeNvs nvs;
    DeviceStore store(nvs);

    DeviceStore::Pending p;
    snprintf(p.update_id, sizeof(p.update_id), "%s", "b3d1-0001");
    snprintf(p.version, sizeof(p.version), "%s", "1.5.0");
    snprintf(p.previous, sizeof(p.previous), "%s", "1.4.2");
    p.started_at_s = 1755698733;
    p.deadline_s   = 1755699033;

    TEST_ASSERT_TRUE(store.savePending(p));
    TEST_ASSERT_TRUE(store.hasPending());

    // Fresh store over the same storage: the reboot.
    DeviceStore after(nvs);
    DeviceStore::Pending loaded;
    TEST_ASSERT_TRUE(after.loadPending(loaded));
    TEST_ASSERT_EQUAL_STRING("b3d1-0001", loaded.update_id);
    TEST_ASSERT_EQUAL_STRING("1.4.2", loaded.previous);
    TEST_ASSERT_EQUAL_UINT32(1755699033, loaded.deadline_s);

    TEST_ASSERT_TRUE(after.clearPending());
    TEST_ASSERT_FALSE(after.hasPending());
}

void test_last_update_id_dedupe_key() {
    FakeNvs nvs;
    DeviceStore store(nvs);

    char id[kMaxIdLen] = {};
    TEST_ASSERT_FALSE(store.lastUpdateId(id, sizeof(id)));

    TEST_ASSERT_TRUE(store.setLastUpdateId("b3d1-0001"));
    TEST_ASSERT_TRUE(store.lastUpdateId(id, sizeof(id)));
    TEST_ASSERT_EQUAL_STRING("b3d1-0001", id);
}

// ------------------------------------------------------------------- state --

void test_ota_state_survives_reboot() {
    FakeNvs nvs;
    DeviceStore store(nvs);

    TEST_ASSERT_EQUAL(OtaState::Idle, store.otaState());
    TEST_ASSERT_TRUE(store.setOtaState(OtaState::PendingUser, AbortReason::None));

    DeviceStore after(nvs);
    TEST_ASSERT_EQUAL(OtaState::PendingUser, after.otaState());

    store.setOtaState(OtaState::Aborted, AbortReason::Gprs);
    TEST_ASSERT_EQUAL(AbortReason::Gprs, after.abortReason());
}

void test_factory_reset_wipes_everything() {
    FakeNvs nvs;
    DeviceStore store(nvs);

    store.seedBootstrapUrl("https://updater.example.com");
    store.saveProvisioning(makeProvisioning());
    store.commitFirmware("1.5.0", "repo");

    TEST_ASSERT_TRUE(store.factoryReset());
    TEST_ASSERT_FALSE(store.isProvisioned());
    TEST_ASSERT_FALSE(store.hasBootstrapUrl());

    char v[kMaxVersionLen] = {};
    TEST_ASSERT_FALSE(store.firmwareVersion(v, sizeof(v)));
}

// Confidentiality comes from NVS + flash encryption, not from this layer, so
// the device must be able to report honestly whether it is actually on.
void test_encryption_is_reported_not_assumed() {
    FakeNvs nvs;
    DeviceStore store(nvs);
    TEST_ASSERT_FALSE(store.encrypted());

    nvs.encryption_on = true;
    TEST_ASSERT_TRUE(store.encrypted());
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_bootstrap_url_is_forgotten_after_provisioning);
    RUN_TEST(test_bootstrap_url_falls_back_to_compiled_default);
    RUN_TEST(test_seeded_bootstrap_url_beats_default);

    RUN_TEST(test_provisioning_round_trip);
    RUN_TEST(test_provisioning_rejects_incomplete_response);
    RUN_TEST(test_failed_write_does_not_mark_provisioned);
    RUN_TEST(test_flag_without_device_id_is_not_provisioned);

    RUN_TEST(test_firmware_version_commit);
    RUN_TEST(test_config_version_only_moves_forward);

    RUN_TEST(test_pending_update_survives_reboot);
    RUN_TEST(test_last_update_id_dedupe_key);

    RUN_TEST(test_ota_state_survives_reboot);
    RUN_TEST(test_factory_reset_wipes_everything);
    RUN_TEST(test_encryption_is_reported_not_assumed);

    return UNITY_END();
}
