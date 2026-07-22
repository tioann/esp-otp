// Persistent TOTP secret store, backed by NVS.
//
// Records live in a single NVS blob, mirrored in RAM for fast cycling. When
// the build enables flash encryption + encrypted NVS, the secret bytes are
// encrypted at rest. Raw secrets never leave the device — the protocol LIST
// command exposes labels/metadata only.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "totp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORE_LABEL_MAX  31   // excluding null terminator
#define STORE_SECRET_MAX 64

typedef struct {
    uint16_t id;                          // stable identifier
    char label[STORE_LABEL_MAX + 1];
    uint8_t secret[STORE_SECRET_MAX];     // raw (base32-decoded) key
    uint8_t secret_len;
    uint8_t digits;                       // typically 6
    uint16_t period;                      // seconds, typically 30
    totp_algo_t algo;
} store_record_t;

// Load records from NVS (creating the namespace on first run).
esp_err_t store_init(void);

size_t store_count(void);
size_t store_capacity(void);

// Fetch by cycle position (0..count-1). Returns false if out of range.
bool store_get(size_t index, store_record_t *out);

// Fetch by stable id. Returns false if not found.
bool store_get_by_id(uint16_t id, store_record_t *out);

// Add a record; assigns a fresh id (written to *out_id). Returns
// ESP_ERR_NO_MEM if full, ESP_ERR_INVALID_ARG on bad input.
esp_err_t store_add(const char *label, const uint8_t *secret, size_t secret_len,
                    uint8_t digits, uint16_t period, totp_algo_t algo, uint16_t *out_id);

// Remove by id. Returns ESP_ERR_NOT_FOUND if absent.
esp_err_t store_remove(uint16_t id);

// Rename by id. Returns ESP_ERR_NOT_FOUND if absent.
esp_err_t store_rename(uint16_t id, const char *label);

// Move the record with `id` to cycle position `new_index` (0..count-1),
// shifting the intervening records to close/open the gap. Returns
// ESP_ERR_NOT_FOUND if absent, ESP_ERR_INVALID_ARG if new_index is out of range.
esp_err_t store_move(uint16_t id, size_t new_index);

#ifdef __cplusplus
}
#endif
