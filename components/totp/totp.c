#include "totp.h"

#include "mbedtls/md.h"

static mbedtls_md_type_t md_type(totp_algo_t algo)
{
    switch (algo) {
        case TOTP_SHA256: return MBEDTLS_MD_SHA256;
        case TOTP_SHA512: return MBEDTLS_MD_SHA512;
        case TOTP_SHA1:
        default:          return MBEDTLS_MD_SHA1;
    }
}

uint32_t totp_compute(const uint8_t *key, size_t key_len, uint64_t unix_time,
                      uint32_t period, uint32_t digits, totp_algo_t algo)
{
    if (period == 0) {
        period = 30;
    }
    if (digits == 0 || digits > 9) {
        digits = 6;
    }

    // RFC 6238: counter = floor(time / period), as a big-endian 8-byte block.
    uint64_t counter = unix_time / period;
    uint8_t msg[8];
    for (int i = 7; i >= 0; i--) {
        msg[i] = (uint8_t)(counter & 0xFF);
        counter >>= 8;
    }

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md_type(algo));
    uint8_t mac[64];  // SHA-512 is the largest at 64 bytes
    if (mbedtls_md_hmac(info, key, key_len, msg, sizeof(msg), mac) != 0) {
        return 0;
    }
    size_t mac_len = mbedtls_md_get_size(info);

    // RFC 4226 dynamic truncation.
    int offset = mac[mac_len - 1] & 0x0F;
    uint32_t bin = ((uint32_t)(mac[offset] & 0x7F) << 24) |
                   ((uint32_t)mac[offset + 1] << 16) |
                   ((uint32_t)mac[offset + 2] << 8) |
                   ((uint32_t)mac[offset + 3]);

    uint32_t mod = 1;
    for (uint32_t i = 0; i < digits; i++) {
        mod *= 10;
    }
    return bin % mod;
}

esp_err_t base32_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint32_t buffer = 0;
    int bits = 0;
    size_t n = 0;

    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '-' || c == '=') {
            continue;  // ignore separators and padding
        }

        int val;
        if (c >= 'A' && c <= 'Z') {
            val = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            val = c - 'a';
        } else if (c >= '2' && c <= '7') {
            val = c - '2' + 26;
        } else {
            return ESP_ERR_INVALID_ARG;
        }

        buffer = (buffer << 5) | (uint32_t)val;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_cap) {
                return ESP_ERR_INVALID_ARG;
            }
            out[n++] = (uint8_t)((buffer >> bits) & 0xFF);
        }
    }

    if (out_len) {
        *out_len = n;
    }
    return ESP_OK;
}
