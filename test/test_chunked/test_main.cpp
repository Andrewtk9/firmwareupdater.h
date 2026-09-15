#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "core/ChunkedDecoder.h"
#include "core/ProvisioningCodec.h"

using namespace campodata;

void setUp() {}
void tearDown() {}

namespace {

// Captured byte for byte from the deployed API: a POST with no board_id, so
// nothing was provisioned. Every JSON answer arrives like this - nginx in front
// of Spring, chunked, no Content-Length.
const char kRealBody[] =
    "6e\r\n"
    "{\"timestamp\":\"2026-09-15T15:41:41.302+00:00\",\"status\":400,\"error\":\"Bad Request\",\"path\":\"/api/v1/provisioning\"}\r\n"
    "0\r\n"
    "\r\n";

const char kRealJson[] = "{\"timestamp\":\"2026-09-15T15:41:41.302+00:00\",\"status\":400,\"error\":\"Bad Request\",\"path\":\"/api/v1/provisioning\"}";

// Drives the decoder the way GsmHttpClient does - payload in bulk while
// inData(), framing one byte at a time - with input arriving in pieces of
// `step` bytes, as the modem FIFO hands them over.
size_t decodeAll(ChunkedDecoder& dec, const char* raw, size_t len,
                 char* out, size_t cap, size_t step) {
    size_t pos     = 0;
    size_t written = 0;

    while (pos < len && !dec.done() && !dec.invalid()) {
        const size_t piece_end = (len - pos > step) ? pos + step : len;

        while (pos < piece_end && !dec.done() && !dec.invalid()) {
            if (!dec.inData()) {
                dec.feed(raw[pos++]);
                continue;
            }
            size_t n = piece_end - pos;
            if (n > dec.dataRemaining()) n = dec.dataRemaining();
            TEST_ASSERT_TRUE_MESSAGE(written + n < cap, "test buffer too small");
            memcpy(out + written, raw + pos, n);
            written += n;
            pos     += n;
            dec.consumeData(static_cast<uint32_t>(n));
        }
    }

    out[written] = '\0';
    return written;
}

// Frames `json` as two chunks split at `cut`, as a server flushing its buffer
// mid-document would.
size_t frameInTwo(const char* json, size_t cut, char* out, size_t cap) {
    const size_t len = strlen(json);
    const int n = snprintf(out, cap, "%lx\r\n%.*s\r\n%lx\r\n%s\r\n0\r\n\r\n",
                           static_cast<unsigned long>(cut), static_cast<int>(cut), json,
                           static_cast<unsigned long>(len - cut), json + cut);
    TEST_ASSERT_TRUE(n > 0 && static_cast<size_t>(n) < cap);
    return static_cast<size_t>(n);
}

}  // namespace

void test_real_server_body_decodes_in_any_piece_size() {
    const size_t steps[] = {1, 2, 3, 7, 64, sizeof(kRealBody) - 1};
    for (size_t step : steps) {
        ChunkedDecoder dec;
        char out[256];
        const size_t n = decodeAll(dec, kRealBody, sizeof(kRealBody) - 1,
                                   out, sizeof(out), step);
        TEST_ASSERT_TRUE(dec.done());
        TEST_ASSERT_FALSE(dec.invalid());
        TEST_ASSERT_EQUAL_UINT(strlen(kRealJson), n);
        TEST_ASSERT_EQUAL_STRING(kRealJson, out);
    }
}

// The field failure: the 200 was parsed with the framing still in it, the codec
// rejected it and nothing was saved - while the server, which delivers
// credentials exactly once, had already locked the board.
void test_provisioning_response_only_parses_after_decoding() {
    const char json[] =
        "{\"device_id\":\"9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de\","
        "\"mqtt\":{\"host\":\"iot.example.com\",\"port\":1883,\"tls\":false,"
        "\"username\":\"dev_9f1c\",\"password\":\"K7c!x2Qm\","
        "\"topics\":{\"ping\":\"/ping/9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de\","
        "\"update\":\"/update/9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de\","
        "\"config\":\"/config/9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de\"}},"
        "\"api_base_url\":\"http://updater.example.com\"}";

    char raw[1024];
    const size_t raw_len = frameInTwo(json, 97, raw, sizeof(raw));

    Provisioning p;
    TEST_ASSERT_NOT_EQUAL(CodecError::Ok, provisioning::parseResponse(raw, p));

    ChunkedDecoder dec;
    char body[1024];
    decodeAll(dec, raw, raw_len, body, sizeof(body), 16);
    TEST_ASSERT_TRUE(dec.done());
    TEST_ASSERT_EQUAL_STRING(json, body);

    TEST_ASSERT_EQUAL(CodecError::Ok, provisioning::parseResponse(body, p));
    TEST_ASSERT_EQUAL_STRING("9f1c2e6a-4b77-4c1e-9c25-0f2c1a8b77de", p.device_id);
    TEST_ASSERT_EQUAL_UINT16(1883, p.mqtt_port);
    TEST_ASSERT_FALSE(p.mqtt_tls);
    TEST_ASSERT_EQUAL_STRING("dev_9f1c", p.mqtt_user);
}

void test_extensions_and_trailers_are_skipped() {
    const char raw[] = "5;name=value\r\nhello\r\n6\r\n world\r\n0\r\nX-Checksum: abc\r\n\r\n";
    ChunkedDecoder dec;
    char out[64];
    decodeAll(dec, raw, sizeof(raw) - 1, out, sizeof(out), 1);
    TEST_ASSERT_TRUE(dec.done());
    TEST_ASSERT_EQUAL_STRING("hello world", out);
}

void test_hex_sizes_in_either_case() {
    const char raw[] = "A\r\n0123456789\r\nb\r\nabcdefghijk\r\n0\r\n\r\n";
    ChunkedDecoder dec;
    char out[64];
    decodeAll(dec, raw, sizeof(raw) - 1, out, sizeof(out), 5);
    TEST_ASSERT_TRUE(dec.done());
    TEST_ASSERT_EQUAL_STRING("0123456789abcdefghijk", out);
}

// A connection that closes mid-body must not look complete: for firmware that
// would be a short image reported as end of file.
void test_truncated_body_is_neither_done_nor_invalid() {
    const char mid_data[] = "a\r\n01234";
    ChunkedDecoder dec;
    char out[64];
    decodeAll(dec, mid_data, sizeof(mid_data) - 1, out, sizeof(out), 3);
    TEST_ASSERT_FALSE(dec.done());
    TEST_ASSERT_FALSE(dec.invalid());
    TEST_ASSERT_TRUE(dec.inData());
    TEST_ASSERT_EQUAL_UINT32(5, dec.dataRemaining());
    TEST_ASSERT_EQUAL_STRING("01234", out);

    const char no_final_crlf[] = "3\r\nabc\r\n0\r\n";
    ChunkedDecoder dec2;
    decodeAll(dec2, no_final_crlf, sizeof(no_final_crlf) - 1, out, sizeof(out), 1);
    TEST_ASSERT_FALSE(dec2.done());
    TEST_ASSERT_FALSE(dec2.invalid());
}

void test_malformed_framing_is_invalid() {
    const char* const cases[] = {
        "zz\r\nabc\r\n0\r\n\r\n",   // size is not hex
        "\r\nabc\r\n0\r\n\r\n",     // empty size line
        "3\r\nabcX\r\n0\r\n\r\n",   // more data than declared
        "FFFFFFFFF\r\n",            // does not fit 32 bits
        ";ext\r\nabc\r\n",          // extension with no size
    };
    for (const char* raw : cases) {
        ChunkedDecoder dec;
        char out[64];
        decodeAll(dec, raw, strlen(raw), out, sizeof(out), 1);
        TEST_ASSERT_TRUE_MESSAGE(dec.invalid(), raw);
        TEST_ASSERT_FALSE(dec.done());
    }
}

void test_reset_starts_a_new_body() {
    ChunkedDecoder dec;
    char out[64];

    const char bad[] = "zz\r\n";
    decodeAll(dec, bad, sizeof(bad) - 1, out, sizeof(out), 1);
    TEST_ASSERT_TRUE(dec.invalid());

    dec.reset();
    const char good[] = "2\r\nhi\r\n0\r\n\r\n";
    decodeAll(dec, good, sizeof(good) - 1, out, sizeof(out), 4);
    TEST_ASSERT_TRUE(dec.done());
    TEST_ASSERT_EQUAL_STRING("hi", out);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_real_server_body_decodes_in_any_piece_size);
    RUN_TEST(test_provisioning_response_only_parses_after_decoding);
    RUN_TEST(test_extensions_and_trailers_are_skipped);
    RUN_TEST(test_hex_sizes_in_either_case);
    RUN_TEST(test_truncated_body_is_neither_done_nor_invalid);
    RUN_TEST(test_malformed_framing_is_invalid);
    RUN_TEST(test_reset_starts_a_new_body);

    return UNITY_END();
}
