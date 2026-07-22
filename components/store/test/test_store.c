// store_move ordering tests. NVS-backed, so they run on-target under QEMU.
#include <string.h>

#include "esp_err.h"
#include "nvs_flash.h"
#include "store.h"
#include "unity.h"

// A minimal valid raw secret; contents don't matter for ordering tests.
static const uint8_t SECRET[] = {0x01, 0x02, 0x03, 0x04, 0x05};

// Reset the store to a known 3-record set A,B,C and return their ids.
static void seed_abc(uint16_t ids[3])
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        TEST_ESP_OK(nvs_flash_erase());
        TEST_ESP_OK(nvs_flash_init());
    } else {
        TEST_ESP_OK(err);
    }
    // Wipe any prior records so ordering assertions start clean.
    nvs_flash_erase();
    TEST_ESP_OK(nvs_flash_init());
    TEST_ESP_OK(store_init());

    TEST_ASSERT_EQUAL_UINT(0, store_count());
    TEST_ESP_OK(store_add("A", SECRET, sizeof(SECRET), 6, 30, TOTP_SHA1, &ids[0]));
    TEST_ESP_OK(store_add("B", SECRET, sizeof(SECRET), 6, 30, TOTP_SHA1, &ids[1]));
    TEST_ESP_OK(store_add("C", SECRET, sizeof(SECRET), 6, 30, TOTP_SHA1, &ids[2]));
    TEST_ASSERT_EQUAL_UINT(3, store_count());
}

static void assert_order(const char *expect)
{
    for (size_t i = 0; expect[i]; i++) {
        store_record_t r;
        TEST_ASSERT_TRUE(store_get(i, &r));
        TEST_ASSERT_EQUAL_CHAR(expect[i], r.label[0]);
    }
}

TEST_CASE("store_move front to back", "[store]")
{
    uint16_t ids[3];
    seed_abc(ids);
    TEST_ESP_OK(store_move(ids[0], 2));   // A -> index 2
    assert_order("BCA");
}

TEST_CASE("store_move back to front", "[store]")
{
    uint16_t ids[3];
    seed_abc(ids);
    TEST_ESP_OK(store_move(ids[2], 0));   // C -> index 0
    assert_order("CAB");
}

TEST_CASE("store_move middle down", "[store]")
{
    uint16_t ids[3];
    seed_abc(ids);
    TEST_ESP_OK(store_move(ids[1], 2));   // B -> index 2
    assert_order("ACB");
}

TEST_CASE("store_move to same index is a no-op", "[store]")
{
    uint16_t ids[3];
    seed_abc(ids);
    TEST_ESP_OK(store_move(ids[1], 1));
    assert_order("ABC");
}

TEST_CASE("store_move rejects bad id and out-of-range index", "[store]")
{
    uint16_t ids[3];
    seed_abc(ids);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, store_move(0xBEEF, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, store_move(ids[0], 3));
    assert_order("ABC");  // unchanged
}
