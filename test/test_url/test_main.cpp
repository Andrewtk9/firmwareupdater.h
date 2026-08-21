#include <unity.h>

#include <string.h>

#include "campodata/Config.h"
#include "core/Url.h"

using namespace campodata;
using namespace campodata::url;

void setUp() {}
void tearDown() {}

void test_scheme_detection() {
    TEST_ASSERT_EQUAL(Scheme::Https, scheme("https://updater.example.com/x"));
    TEST_ASSERT_EQUAL(Scheme::Http,  scheme("http://updater.example.com/x"));
    TEST_ASSERT_EQUAL(Scheme::Https, scheme("HTTPS://UPDATER.PLUG.FARM/x"));
    TEST_ASSERT_EQUAL(Scheme::Unknown, scheme("ftp://x"));
    TEST_ASSERT_EQUAL(Scheme::Unknown, scheme(nullptr));
}

// The server builds every download URL from one configured base, so a cellular
// device really can be handed an https:// URL it cannot open.
void test_cellular_cannot_fetch_https() {
    const char* https_url = "https://updater.example.com/firmware/abc";
    const char* http_url  = "http://updater.example.com/firmware/abc";

    // Wi-Fi: both fine.
    TEST_ASSERT_TRUE(fetchable(https_url, true));
    TEST_ASSERT_TRUE(fetchable(http_url, true));

    // GPRS on Arduino: no TLS at all.
    TEST_ASSERT_FALSE(fetchable(https_url, false));
    TEST_ASSERT_TRUE(fetchable(http_url, false));
}

void test_unknown_scheme_is_never_fetchable() {
    TEST_ASSERT_FALSE(fetchable("updater.example.com/x", true));
    TEST_ASSERT_FALSE(fetchable(nullptr, true));
}

void test_join_normalises_separator() {
    char out[128];
    const char* base = "https://updater.example.com";

    TEST_ASSERT_TRUE(join(base, "/api/v1/provisioning", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://updater.example.com/api/v1/provisioning", out);

    // Trailing slash on the base must not double up.
    TEST_ASSERT_TRUE(join("https://updater.example.com/", "/api/v1/provisioning",
                          out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://updater.example.com/api/v1/provisioning", out);

    // Missing leading slash on the path must not concatenate.
    TEST_ASSERT_TRUE(join(base, "api/v1/provisioning", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://updater.example.com/api/v1/provisioning", out);
}

void test_join_rejects_small_buffer() {
    char out[8];
    TEST_ASSERT_FALSE(join("https://updater.example.com", "/api/v1/provisioning",
                           out, sizeof(out)));
}

// Paths are the library's business; hosts are the project's.
void test_confirm_path_matches_spec() {
    EndpointConfig ep;
    char out[192];
    TEST_ASSERT_TRUE(formatPath("https://srv", ep.path_confirm,
                                "b3d1aaaa-0000-4000-8000-000000000001",
                                out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(
        "https://srv/api/v1/firmware/"
        "b3d1aaaa-0000-4000-8000-000000000001/confirm", out);
}

void test_provisioning_path_matches_spec() {
    EndpointConfig ep;
    char out[192];
    TEST_ASSERT_TRUE(join("https://srv", ep.path_provisioning, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://srv/api/v1/provisioning", out);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_scheme_detection);
    RUN_TEST(test_cellular_cannot_fetch_https);
    RUN_TEST(test_unknown_scheme_is_never_fetchable);
    RUN_TEST(test_join_normalises_separator);
    RUN_TEST(test_join_rejects_small_buffer);
    RUN_TEST(test_confirm_path_matches_spec);
    RUN_TEST(test_provisioning_path_matches_spec);

    return UNITY_END();
}
