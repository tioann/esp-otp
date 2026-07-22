#include "store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "store";

#define STORE_NS   "otp"
#define KEY_RECS   "recs"
#define KEY_NEXTID "nextid"

#define STORE_CAPACITY CONFIG_OTP_STORE_CAPACITY

// Whole table held in RAM; NVS is rewritten on every mutation. Fine for a
// few dozen records.
static store_record_t s_recs[STORE_CAPACITY];
static size_t s_count;
static uint16_t s_next_id = 1;

static esp_err_t persist(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(STORE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY_RECS, s_recs, s_count * sizeof(s_recs[0]));
    if (err == ESP_OK) {
        err = nvs_set_u16(h, KEY_NEXTID, s_next_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t store_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(STORE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = sizeof(s_recs);
    err = nvs_get_blob(h, KEY_RECS, s_recs, &size);
    if (err == ESP_OK) {
        s_count = size / sizeof(s_recs[0]);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_count = 0;   // first boot
        err = ESP_OK;
    }

    uint16_t next = 1;
    if (nvs_get_u16(h, KEY_NEXTID, &next) == ESP_OK) {
        s_next_id = next;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "loaded %u/%u secrets", (unsigned)s_count, (unsigned)STORE_CAPACITY);
    return err;
}

size_t store_count(void) { return s_count; }
size_t store_capacity(void) { return STORE_CAPACITY; }

bool store_get(size_t index, store_record_t *out)
{
    if (index >= s_count) {
        return false;
    }
    *out = s_recs[index];
    return true;
}

static int find_index(uint16_t id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_recs[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

bool store_get_by_id(uint16_t id, store_record_t *out)
{
    int idx = find_index(id);
    if (idx < 0) {
        return false;
    }
    *out = s_recs[idx];
    return true;
}

esp_err_t store_add(const char *label, const uint8_t *secret, size_t secret_len,
                    uint8_t digits, uint16_t period, totp_algo_t algo, uint16_t *out_id)
{
    if (label == NULL || secret == NULL || secret_len == 0 ||
        secret_len > STORE_SECRET_MAX || strlen(label) > STORE_LABEL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count >= STORE_CAPACITY) {
        return ESP_ERR_NO_MEM;
    }

    store_record_t *r = &s_recs[s_count];
    memset(r, 0, sizeof(*r));
    r->id = s_next_id++;
    strlcpy(r->label, label, sizeof(r->label));
    memcpy(r->secret, secret, secret_len);
    r->secret_len = (uint8_t)secret_len;
    r->digits = digits ? digits : 6;
    r->period = period ? period : 30;
    r->algo = algo;

    s_count++;
    esp_err_t err = persist();
    if (err != ESP_OK) {
        s_count--;          // roll back on persistence failure
        s_next_id--;
        return err;
    }
    if (out_id) {
        *out_id = r->id;
    }
    return ESP_OK;
}

esp_err_t store_remove(uint16_t id)
{
    int idx = find_index(id);
    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    // Compact the array to preserve cycle order.
    for (size_t i = idx; i + 1 < s_count; i++) {
        s_recs[i] = s_recs[i + 1];
    }
    s_count--;
    memset(&s_recs[s_count], 0, sizeof(s_recs[0]));
    return persist();
}

esp_err_t store_rename(uint16_t id, const char *label)
{
    if (label == NULL || strlen(label) > STORE_LABEL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    int idx = find_index(id);
    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(s_recs[idx].label, label, sizeof(s_recs[idx].label));
    return persist();
}

esp_err_t store_move(uint16_t id, size_t new_index)
{
    if (new_index >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }
    int from = find_index(id);
    if (from < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t to = new_index;
    if ((size_t)from == to) {
        return ESP_OK;   // no-op, avoid a needless flash write
    }

    store_record_t moved = s_recs[from];
    if ((size_t)from < to) {
        for (size_t i = from; i < to; i++) {
            s_recs[i] = s_recs[i + 1];
        }
    } else {
        for (size_t i = from; i > to; i--) {
            s_recs[i] = s_recs[i - 1];
        }
    }
    s_recs[to] = moved;
    return persist();
}
