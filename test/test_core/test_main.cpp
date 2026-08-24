#include <unity.h>

#include <stdio.h>

#include "campodata/Types.h"
#include "core/Backoff.h"
#include "core/HardwareModel.h"

using namespace campodata;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- Backoff ---

void test_backoff_ready_before_any_failure() {
    Backoff b;
    b.configure(backoff::kMqtt, backoff::kMqttCount, 0);
    TEST_ASSERT_TRUE(b.ready(0));
    TEST_ASSERT_EQUAL_UINT8(0, b.attempts());
}

void test_backoff_blocks_then_releases() {
    Backoff b;
    b.configure(backoff::kMqtt, backoff::kMqttCount, 0);  // 3000, 8000, 15000

    b.fail(1000);
    TEST_ASSERT_FALSE(b.ready(1000));
    TEST_ASSERT_FALSE(b.ready(3999));
    TEST_ASSERT_TRUE(b.ready(4000));
}

void test_backoff_escalates() {
    Backoff b;
    b.configure(backoff::kMqtt, backoff::kMqttCount, 0);

    b.fail(0);
    TEST_ASSERT_EQUAL_UINT32(3000, b.currentDelayMs());
    b.fail(0);
    TEST_ASSERT_EQUAL_UINT32(8000, b.currentDelayMs());
    b.fail(0);
    TEST_ASSERT_EQUAL_UINT32(15000, b.currentDelayMs());
}

void test_backoff_saturates_at_last_step() {
    Backoff b;
    b.configure(backoff::kMqtt, backoff::kMqttCount, 0);
    for (int i = 0; i < 20; ++i) b.fail(0);
    TEST_ASSERT_EQUAL_UINT32(15000, b.currentDelayMs());
}

void test_backoff_reset_returns_to_first_step() {
    Backoff b;
    b.configure(backoff::kMqtt, backoff::kMqttCount, 0);
    b.fail(0);
    b.fail(0);
    b.reset();
    TEST_ASSERT_TRUE(b.ready(0));
    TEST_ASSERT_EQUAL_UINT8(0, b.attempts());
    b.fail(0);
    TEST_ASSERT_EQUAL_UINT32(3000, b.currentDelayMs());
}

// A 49-day uptime must not wedge the retry loop forever.
void test_backoff_survives_millis_rollover() {
    Backoff b;
    b.configure(backoff::kMqtt, backoff::kMqttCount, 0);

    const uint32_t near_wrap = 0xFFFFFF00u;
    b.fail(near_wrap);
    TEST_ASSERT_FALSE(b.ready(near_wrap + 100));

    const uint32_t after_wrap = near_wrap + 3000;  // wraps around zero
    TEST_ASSERT_TRUE(b.ready(after_wrap));
}

void test_backoff_jitter_stays_in_band() {
    Backoff b;
    b.configure(backoff::kNetwork, backoff::kNetworkCount, 20);  // first step 2000

    for (uint32_t t = 0; t < 500; ++t) {
        b.reset();
        b.fail(t * 7919u);
        const uint32_t d = b.currentDelayMs();
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1600, d);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(2400, d);
    }
}

void test_backoff_jitter_actually_varies() {
    Backoff b;
    b.configure(backoff::kNetwork, backoff::kNetworkCount, 20);

    b.reset(); b.fail(1234);
    const uint32_t a = b.currentDelayMs();

    bool differs = false;
    for (uint32_t t = 1235; t < 1300 && !differs; ++t) {
        b.reset();
        b.fail(t);
        if (b.currentDelayMs() != a) differs = true;
    }
    TEST_ASSERT_TRUE(differs);
}

// ---------------------------------------------------------- HardwareModel ---

HardwareInfo makeHw(const char* chip, uint32_t flash_mb, uint32_t psram_mb) {
    HardwareInfo hw;
    snprintf(hw.chip, sizeof(hw.chip), "%s", chip);
    hw.flash_bytes = flash_mb * 1024u * 1024u;
    hw.psram_bytes = psram_mb * 1024u * 1024u;
    return hw;
}

// The case no compile-time macro can decide: same silicon, PSRAM is the tell.
void test_classify_separates_wrover_from_wroom() {
    char out[kMaxModelLen];

    const HardwareInfo wroom = makeHw("ESP32", 4, 0);
    TEST_ASSERT_TRUE(hardware::classify(wroom, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("esp32-wroom-n4", out);

    const HardwareInfo wrover = makeHw("ESP32", 4, 8);
    TEST_ASSERT_TRUE(hardware::classify(wrover, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("esp32-wrover-n4r8", out);
}

void test_classify_s3_variants() {
    char out[kMaxModelLen];

    const HardwareInfo s3_psram = makeHw("ESP32-S3", 4, 2);
    TEST_ASSERT_TRUE(hardware::classify(s3_psram, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("esp32s3-n4r2", out);

    const HardwareInfo s3_plain = makeHw("ESP32-S3", 4, 0);
    TEST_ASSERT_TRUE(hardware::classify(s3_plain, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("esp32s3-n4", out);
}

// Real getChipModel() strings carry a package suffix.
void test_classify_handles_full_chip_strings() {
    char out[kMaxModelLen];
    const HardwareInfo hw = makeHw("ESP32-D0WD-V3", 4, 0);
    TEST_ASSERT_TRUE(hardware::classify(hw, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("esp32-wroom-n4", out);
}

void test_classify_unknown_chip_falls_back() {
    char out[kMaxModelLen];
    const HardwareInfo hw = makeHw("ESP32-C6", 8, 0);
    TEST_ASSERT_TRUE(hardware::classify(hw, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("esp32-c6-n8", out);
}

void test_classify_rejects_tiny_buffer() {
    char out[4];
    const HardwareInfo hw = makeHw("ESP32", 4, 8);
    TEST_ASSERT_FALSE(hardware::classify(hw, out, sizeof(out)));
}

// Must reproduce the fleet's sprintf("%04X%08X") byte order exactly, or every
// already-registered device is orphaned.
void test_board_id_matches_fleet_format() {
    const uint8_t mac[6] = {0xA8, 0x46, 0x74, 0x9E, 0x4C, 0x40};
    char out[13];
    TEST_ASSERT_TRUE(hardware::boardId(mac, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("404C9E7446A8", out);
}

void test_board_id_rejects_small_buffer() {
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    char out[12];
    TEST_ASSERT_FALSE(hardware::boardId(mac, out, sizeof(out)));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_backoff_ready_before_any_failure);
    RUN_TEST(test_backoff_blocks_then_releases);
    RUN_TEST(test_backoff_escalates);
    RUN_TEST(test_backoff_saturates_at_last_step);
    RUN_TEST(test_backoff_reset_returns_to_first_step);
    RUN_TEST(test_backoff_survives_millis_rollover);
    RUN_TEST(test_backoff_jitter_stays_in_band);
    RUN_TEST(test_backoff_jitter_actually_varies);

    RUN_TEST(test_classify_separates_wrover_from_wroom);
    RUN_TEST(test_classify_s3_variants);
    RUN_TEST(test_classify_handles_full_chip_strings);
    RUN_TEST(test_classify_unknown_chip_falls_back);
    RUN_TEST(test_classify_rejects_tiny_buffer);
    RUN_TEST(test_board_id_matches_fleet_format);
    RUN_TEST(test_board_id_rejects_small_buffer);

    return UNITY_END();
}
