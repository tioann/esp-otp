#include <string.h>

#include "totp.h"
#include "unity.h"

// RFC 6238 Appendix B reference vectors (8-digit codes, T0=0, period=30).
static void expect(const char *seed, uint64_t t, totp_algo_t algo, uint32_t exp)
{
    uint32_t got = totp_compute((const uint8_t *)seed, strlen(seed), t, 30, 8, algo);
    TEST_ASSERT_EQUAL_UINT32(exp, got);
}

#define SEED_SHA1   "12345678901234567890"
#define SEED_SHA256 "12345678901234567890123456789012"
#define SEED_SHA512 "1234567890123456789012345678901234567890123456789012345678901234"

TEST_CASE("RFC6238 vectors", "[totp]")
{
    expect(SEED_SHA1, 59, TOTP_SHA1, 94287082);
    expect(SEED_SHA256, 59, TOTP_SHA256, 46119246);
    expect(SEED_SHA512, 59, TOTP_SHA512, 90693936);
    expect(SEED_SHA1, 1111111109, TOTP_SHA1, 7081804);
    expect(SEED_SHA1, 1111111111, TOTP_SHA1, 14050471);
    expect(SEED_SHA1, 1234567890, TOTP_SHA1, 89005924);
    expect(SEED_SHA1, 2000000000, TOTP_SHA1, 69279037);
    expect(SEED_SHA1, 20000000000ULL, TOTP_SHA1, 65353130);
}

TEST_CASE("base32 decode", "[totp]")
{
    uint8_t out[32];
    size_t n = 0;

    // The SHA1 seed, base32-encoded, must round-trip and reproduce the code.
    TEST_ASSERT_EQUAL(ESP_OK,
        base32_decode("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_UINT(20, n);
    TEST_ASSERT_EQUAL_UINT32(94287082, totp_compute(out, n, 59, 30, 8, TOTP_SHA1));

    // Case/space tolerance and padding.
    TEST_ASSERT_EQUAL(ESP_OK,
        base32_decode("jbsw y3dp ee======", out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_UINT(6, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY("Hello!", out, 6);

    // Invalid character is rejected.
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        base32_decode("0189", out, sizeof(out), &n));
}
