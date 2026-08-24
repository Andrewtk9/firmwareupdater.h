#include <unity.h>

#include "campodata/Types.h"
#include "core/Hex.h"
#include "core/Iso8601.h"

using namespace campodata;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- Iso8601 ---

void test_epoch_roundtrip() {
    const int64_t epoch = 1755698733;  // 2025-08-20T14:05:33Z
    iso8601::Civil c;
    TEST_ASSERT_TRUE(iso8601::fromEpoch(epoch, c));
    TEST_ASSERT_EQUAL_INT32(2025, c.year);
    TEST_ASSERT_EQUAL_UINT8(8, c.month);
    TEST_ASSERT_EQUAL_UINT8(20, c.day);
    TEST_ASSERT_EQUAL_INT64(epoch, iso8601::toEpoch(c));
}

void test_format_matches_spec() {
    char buf[iso8601::kBufferLen];
    TEST_ASSERT_TRUE(iso8601::format(1755698733, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("2025-08-20T14:05:33Z", buf);
}

void test_format_rejects_short_buffer() {
    char buf[8];
    TEST_ASSERT_FALSE(iso8601::format(1755698733, buf, sizeof(buf)));
}

void test_leap_day() {
    char buf[iso8601::kBufferLen];
    TEST_ASSERT_TRUE(iso8601::format(1709164800, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("2024-02-29T00:00:00Z", buf);
}

void test_parse_z_form() {
    int64_t e = 0;
    TEST_ASSERT_TRUE(iso8601::parse("2025-08-20T14:05:33Z", e));
    TEST_ASSERT_EQUAL_INT64(1755698733, e);
}

// The whole fleet emits this shape today, so the parser has to accept it.
void test_parse_legacy_space_form() {
    int64_t e = 0;
    TEST_ASSERT_TRUE(iso8601::parse("2025-08-20 14:05:33", e));
    TEST_ASSERT_EQUAL_INT64(1755698733, e);
}

void test_parse_with_offset() {
    int64_t utc = 0, minus3 = 0;
    TEST_ASSERT_TRUE(iso8601::parse("2025-08-20T14:05:33Z", utc));
    TEST_ASSERT_TRUE(iso8601::parse("2025-08-20T11:05:33-03:00", minus3));
    TEST_ASSERT_EQUAL_INT64(utc, minus3);
}

void test_parse_rejects_garbage() {
    int64_t e = 0;
    TEST_ASSERT_FALSE(iso8601::parse("not-a-timestamp", e));
    TEST_ASSERT_FALSE(iso8601::parse("2025-13-20T14:05:33Z", e));
    TEST_ASSERT_FALSE(iso8601::parse("2025-08-20X14:05:33Z", e));
    TEST_ASSERT_FALSE(iso8601::parse("", e));
}

// This is the pluviometro bug: the modem hands back local time plus an offset,
// and discarding the offset silently stores local time as if it were UTC.
void test_gsm_offset_is_applied() {
    iso8601::Civil local;
    local.year = 2025; local.month = 8; local.day = 20;
    local.hour = 11;   local.minute = 5; local.second = 33;

    const int64_t utc = iso8601::fromGsmLocal(local, -12);  // -12 quarters = UTC-3
    char buf[iso8601::kBufferLen];
    TEST_ASSERT_TRUE(iso8601::format(utc, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("2025-08-20T14:05:33Z", buf);
}

void test_gsm_zero_offset_is_identity() {
    iso8601::Civil c;
    c.year = 2025; c.month = 8; c.day = 20;
    c.hour = 14;   c.minute = 5; c.second = 33;
    TEST_ASSERT_EQUAL_INT64(iso8601::toEpoch(c), iso8601::fromGsmLocal(c, 0));
}

void test_plausible_rejects_unsynced_modem() {
    iso8601::Civil sim800_default;
    sim800_default.year = 1980; sim800_default.month = 1; sim800_default.day = 6;
    TEST_ASSERT_FALSE(iso8601::plausible(iso8601::toEpoch(sim800_default)));
    TEST_ASSERT_FALSE(iso8601::plausible(0));
    TEST_ASSERT_TRUE(iso8601::plausible(1755698733));
}

// -------------------------------------------------------------------- Hex ---

void test_hex_decode_sha256() {
    const char* text =
        "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3a94a8fe5ccb19ba61c4c0873";
    uint8_t out[kSha256Bytes] = {};
    TEST_ASSERT_TRUE(hex::decode(text, out, kSha256Bytes));
    TEST_ASSERT_EQUAL_UINT8(0xa9, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x73, out[31]);
}

void test_hex_roundtrip() {
    const char* text =
        "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3a94a8fe5ccb19ba61c4c0873";
    uint8_t raw[kSha256Bytes] = {};
    char back[kSha256HexLen + 1] = {};
    TEST_ASSERT_TRUE(hex::decode(text, raw, kSha256Bytes));
    TEST_ASSERT_TRUE(hex::encode(raw, kSha256Bytes, back, sizeof(back)));
    TEST_ASSERT_EQUAL_STRING(text, back);
}

void test_hex_rejects_bad_input() {
    uint8_t out[4] = {};
    TEST_ASSERT_FALSE(hex::decode("zzzzzzzz", out, 4));   // non-hex
    TEST_ASSERT_FALSE(hex::decode("aabb", out, 4));       // too short
    TEST_ASSERT_FALSE(hex::decode("aabbccddee", out, 4)); // trailing junk
}

void test_hex_equal() {
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[4] = {1, 2, 3, 4};
    const uint8_t c[4] = {1, 2, 3, 5};
    TEST_ASSERT_TRUE(hex::equal(a, b, 4));
    TEST_ASSERT_FALSE(hex::equal(a, c, 4));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_epoch_roundtrip);
    RUN_TEST(test_format_matches_spec);
    RUN_TEST(test_format_rejects_short_buffer);
    RUN_TEST(test_leap_day);
    RUN_TEST(test_parse_z_form);
    RUN_TEST(test_parse_legacy_space_form);
    RUN_TEST(test_parse_with_offset);
    RUN_TEST(test_parse_rejects_garbage);
    RUN_TEST(test_gsm_offset_is_applied);
    RUN_TEST(test_gsm_zero_offset_is_identity);
    RUN_TEST(test_plausible_rejects_unsynced_modem);

    RUN_TEST(test_hex_decode_sha256);
    RUN_TEST(test_hex_roundtrip);
    RUN_TEST(test_hex_rejects_bad_input);
    RUN_TEST(test_hex_equal);

    return UNITY_END();
}
