#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "campodata/Types.h"
#include "core/OrderCodec.h"

using namespace campodata;
using namespace campodata::order;

void setUp() {}
void tearDown() {}

// The exact order payload from spec section 7.1.
static const char* kSpecOrder = R"({
  "update_id": "b3d1aaaa-0000-4000-8000-000000000001",
  "target_version": "1.5.0",
  "repo": "campotech/sensor-firmware",
  "url": "https://updater.example.com/api/v1/firmware/b3d1/download",
  "size_bytes": 1148576,
  "sha256": "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3a94a8fe5ccb19ba61c4c0873",
  "mandatory": false,
  "requires_button": true,
  "issued_at": "2026-08-18T14:10:00Z"
})";

// The real OTA slot on the fleet's shared partition table.
static constexpr uint32_t kSlot = 0x1E0000;  // 1 966 080

// ------------------------------------------------------------------ parse ---

void test_parses_the_spec_order() {
    UpdateOrder o;
    TEST_ASSERT_EQUAL(CodecError::Ok, parse(kSpecOrder, o));

    TEST_ASSERT_EQUAL_STRING("b3d1aaaa-0000-4000-8000-000000000001", o.update_id);
    TEST_ASSERT_EQUAL_STRING("1.5.0", o.target_version);
    TEST_ASSERT_EQUAL_STRING("campotech/sensor-firmware", o.repo);
    TEST_ASSERT_EQUAL_UINT32(1148576, o.size_bytes);
    TEST_ASSERT_TRUE(o.has_sha256);
    TEST_ASSERT_EQUAL_UINT8(0xa9, o.sha256[0]);
    TEST_ASSERT_EQUAL_UINT8(0x73, o.sha256[31]);
    TEST_ASSERT_FALSE(o.mandatory);
    TEST_ASSERT_TRUE(o.requires_button);
    TEST_ASSERT_TRUE(o.issued_at > 0);
}

// requires_button defaults to true: a malformed order must not silently
// bypass the physical confirmation the spec requires.
void test_requires_button_defaults_true() {
    const char* json = R"({"update_id":"a","target_version":"2.0.0",
        "url":"http://x/f.bin","size_bytes":1000,
        "sha256":"a94a8fe5ccb19ba61c4c0873d391e987982fbbd3a94a8fe5ccb19ba61c4c0873"})";
    UpdateOrder o;
    TEST_ASSERT_EQUAL(CodecError::Ok, parse(json, o));
    TEST_ASSERT_TRUE(o.requires_button);
    TEST_ASSERT_FALSE(o.mandatory);
}

void test_parse_rejects_missing_fields() {
    UpdateOrder o;
    TEST_ASSERT_EQUAL(CodecError::MissingField,
                      parse(R"({"target_version":"1.0.0","url":"http://x"})", o));
    TEST_ASSERT_EQUAL(CodecError::MissingField,
                      parse(R"({"update_id":"a","url":"http://x"})", o));
}

void test_parse_rejects_bad_size_and_hash() {
    UpdateOrder o;
    TEST_ASSERT_EQUAL(CodecError::BadValue,
        parse(R"({"update_id":"a","target_version":"1.0","url":"http://x","size_bytes":0})", o));
    TEST_ASSERT_EQUAL(CodecError::BadValue,
        parse(R"({"update_id":"a","target_version":"1.0","url":"http://x",
                 "size_bytes":10,"sha256":"nothex"})", o));
}

void test_parse_rejects_malformed() {
    UpdateOrder o;
    TEST_ASSERT_EQUAL(CodecError::MalformedJson, parse("{oops", o));
    TEST_ASSERT_EQUAL(CodecError::MalformedJson, parse(nullptr, o));
}

// --------------------------------------------------------------- versions ---

void test_version_compare() {
    TEST_ASSERT_TRUE(compareVersions("1.5.0", "1.4.2") > 0);
    TEST_ASSERT_TRUE(compareVersions("1.4.2", "1.5.0") < 0);
    TEST_ASSERT_EQUAL_INT(0, compareVersions("1.4.2", "1.4.2"));
    TEST_ASSERT_TRUE(compareVersions("2.0.0", "1.99.99") > 0);
    TEST_ASSERT_TRUE(compareVersions("1.10.0", "1.9.0") > 0);   // not lexicographic
    TEST_ASSERT_EQUAL_INT(0, compareVersions("1.4", "1.4.0"));  // missing part == 0
}

// --------------------------------------------------------------- validate ---

static UpdateOrder goodOrder() {
    UpdateOrder o;
    parse(kSpecOrder, o);
    return o;
}

void test_valid_order_passes() {
    const UpdateOrder o = goodOrder();
    TEST_ASSERT_EQUAL(OrderRejection::None,
        validate(o, "1.4.2", "campotech/sensor-firmware", kSlot, "", false));
}

// Retained topics replay on every reconnect, and Both-mode migration makes
// reconnects routine. Without this the same update runs forever.
void test_duplicate_order_is_rejected() {
    const UpdateOrder o = goodOrder();
    TEST_ASSERT_EQUAL(OrderRejection::Duplicate,
        validate(o, "1.4.2", "campotech/sensor-firmware", kSlot, o.update_id, false));
}

// Not in the spec, but flashing another project's firmware bricks the device.
void test_order_for_another_repo_is_refused() {
    const UpdateOrder o = goodOrder();
    TEST_ASSERT_EQUAL(OrderRejection::WrongRepo,
        validate(o, "1.4.2", "campotech/pluviometro", kSlot, "", false));
}

void test_same_version_is_refused() {
    const UpdateOrder o = goodOrder();
    TEST_ASSERT_EQUAL(OrderRejection::SameVersion,
        validate(o, "1.5.0", "campotech/sensor-firmware", kSlot, "", false));
}

// No anti-rollback eFuse on this platform, so a valid older image would
// otherwise replay without complaint.
void test_downgrade_refused_unless_allowed() {
    const UpdateOrder o = goodOrder();
    TEST_ASSERT_EQUAL(OrderRejection::Downgrade,
        validate(o, "1.9.0", "campotech/sensor-firmware", kSlot, "", false));
    TEST_ASSERT_EQUAL(OrderRejection::None,
        validate(o, "1.9.0", "campotech/sensor-firmware", kSlot, "", true));
}

// The fleet hardcodes a 2 000 000 cap while the real slot is 1 966 080, so an
// image in between downloads fully over GPRS and only then fails to flash.
void test_image_larger_than_real_slot_is_refused_before_download() {
    UpdateOrder o = goodOrder();
    o.size_bytes = 1990000;  // under the old hardcoded cap, over the real slot
    TEST_ASSERT_EQUAL(OrderRejection::TooLarge,
        validate(o, "1.4.2", "campotech/sensor-firmware", kSlot, "", false));

    o.size_bytes = kSlot;
    TEST_ASSERT_EQUAL(OrderRejection::None,
        validate(o, "1.4.2", "campotech/sensor-firmware", kSlot, "", false));
}

void test_order_without_digest_is_refused() {
    UpdateOrder o = goodOrder();
    o.has_sha256 = false;
    TEST_ASSERT_EQUAL(OrderRejection::NoSha256,
        validate(o, "1.4.2", "campotech/sensor-firmware", kSlot, "", false));
}

// A device that has never been told its repo must not block on that check.
void test_missing_repo_context_does_not_block() {
    const UpdateOrder o = goodOrder();
    TEST_ASSERT_EQUAL(OrderRejection::None,
        validate(o, "1.4.2", "", kSlot, "", false));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_parses_the_spec_order);
    RUN_TEST(test_requires_button_defaults_true);
    RUN_TEST(test_parse_rejects_missing_fields);
    RUN_TEST(test_parse_rejects_bad_size_and_hash);
    RUN_TEST(test_parse_rejects_malformed);

    RUN_TEST(test_version_compare);

    RUN_TEST(test_valid_order_passes);
    RUN_TEST(test_duplicate_order_is_rejected);
    RUN_TEST(test_order_for_another_repo_is_refused);
    RUN_TEST(test_same_version_is_refused);
    RUN_TEST(test_downgrade_refused_unless_allowed);
    RUN_TEST(test_image_larger_than_real_slot_is_refused_before_download);
    RUN_TEST(test_order_without_digest_is_refused);
    RUN_TEST(test_missing_repo_context_does_not_block);

    return UNITY_END();
}
