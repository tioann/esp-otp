// RFC 6238 TOTP engine and RFC 4648 base32 decoding.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOTP_SHA1 = 0,   // default for virtually all authenticators
    TOTP_SHA256 = 1,
    TOTP_SHA512 = 2,
} totp_algo_t;

// Compute a TOTP code. `key`/`key_len` is the raw (already base32-decoded)
// shared secret. `digits` is typically 6 (1..9 supported). The returned
// value is the numeric code; render it zero-padded to `digits`.
uint32_t totp_compute(const uint8_t *key, size_t key_len, uint64_t unix_time,
                      uint32_t period, uint32_t digits, totp_algo_t algo);

// Decode a base32 string (RFC 4648, case-insensitive). Spaces and '='
// padding are ignored. Writes up to `out_cap` bytes and sets `*out_len`.
// Returns ESP_OK, or ESP_ERR_INVALID_ARG on a bad character / overflow.
esp_err_t base32_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
