#include "clocksrc.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"

static const char *TAG = "clocksrc";

#define DS3231_ADDR     0x68
#define DS3231_REG_TIME 0x00  // seconds..year span 7 registers
#define DS3231_REG_STATUS 0x0F
#define DS3231_STATUS_OSF 0x80  // oscillator-stop flag (set => time untrustworthy)

static i2c_master_dev_handle_t s_rtc;   // NULL when no RTC present
static bool s_time_valid;
static clocksrc_sync_source_t s_sync_source;  // CLOCKSRC_SYNC_NONE until first sync
static uint64_t s_last_sync;            // Unix time recorded at the last sync

static uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static esp_err_t rtc_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_rtc, &reg, 1, buf, n, 1000);
}

static esp_err_t rtc_write(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tmp[8];
    if (n + 1 > sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    tmp[0] = reg;
    for (size_t i = 0; i < n; i++) {
        tmp[i + 1] = buf[i];
    }
    return i2c_master_transmit(s_rtc, tmp, n + 1, 1000);
}

// Read the DS3231 time registers into a Unix timestamp. Assumes 24h mode.
static esp_err_t rtc_get(uint64_t *out)
{
    uint8_t r[7];
    esp_err_t err = rtc_read(DS3231_REG_TIME, r, sizeof(r));
    if (err != ESP_OK) {
        return err;
    }
    struct tm tm = {
        .tm_sec = bcd2dec(r[0] & 0x7F),
        .tm_min = bcd2dec(r[1] & 0x7F),
        .tm_hour = bcd2dec(r[2] & 0x3F),
        .tm_mday = bcd2dec(r[4] & 0x3F),
        .tm_mon = bcd2dec(r[5] & 0x1F) - 1,
        .tm_year = bcd2dec(r[6]) + 100,  // DS3231 year is 00..99 from 2000
    };
    // TZ is pinned to UTC in clocksrc_init(), so mktime acts as timegm.
    *out = (uint64_t)mktime(&tm);
    return ESP_OK;
}

static esp_err_t rtc_put(uint64_t unix_time)
{
    time_t t = (time_t)unix_time;
    struct tm tm;
    gmtime_r(&t, &tm);
    uint8_t r[7] = {
        dec2bcd(tm.tm_sec),
        dec2bcd(tm.tm_min),
        dec2bcd(tm.tm_hour),          // bit6 clear => 24h mode
        dec2bcd(tm.tm_wday + 1),
        dec2bcd(tm.tm_mday),
        dec2bcd(tm.tm_mon + 1),       // century bit left clear
        dec2bcd(tm.tm_year - 100),
    };
    esp_err_t err = rtc_write(DS3231_REG_TIME, r, sizeof(r));
    if (err != ESP_OK) {
        return err;
    }
    // Clear the oscillator-stop flag now that we hold a valid time.
    uint8_t status;
    if (rtc_read(DS3231_REG_STATUS, &status, 1) == ESP_OK) {
        status &= ~DS3231_STATUS_OSF;
        rtc_write(DS3231_REG_STATUS, &status, 1);
    }
    return ESP_OK;
}

esp_err_t clocksrc_init(i2c_master_bus_handle_t bus)
{
    setenv("TZ", "UTC0", 1);
    tzset();

    if (bus == NULL) {
        // Simulator: no I2C bus at all; run as if no RTC is fitted.
        ESP_LOGI(TAG, "no I2C bus; time invalid until host sync");
        return ESP_OK;
    }

    if (i2c_master_probe(bus, DS3231_ADDR, 200) != ESP_OK) {
        ESP_LOGI(TAG, "no DS3231; time invalid until host sync");
        return ESP_OK;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_rtc) != ESP_OK) {
        s_rtc = NULL;
        ESP_LOGW(TAG, "DS3231 detected but could not attach");
        return ESP_OK;
    }

    uint8_t status = 0;
    if (rtc_read(DS3231_REG_STATUS, &status, 1) == ESP_OK &&
        !(status & DS3231_STATUS_OSF)) {
        uint64_t now = 0;
        if (rtc_get(&now) == ESP_OK) {
            struct timeval tv = {.tv_sec = (time_t)now};
            settimeofday(&tv, NULL);
            s_time_valid = true;
            s_sync_source = CLOCKSRC_SYNC_RTC;
            s_last_sync = now;
            ESP_LOGI(TAG, "DS3231 present, seeded clock to %llu", (unsigned long long)now);
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "DS3231 present but oscillator stopped; need host sync");
    return ESP_OK;
}

bool clocksrc_rtc_present(void) { return s_rtc != NULL; }
bool clocksrc_time_valid(void) { return s_time_valid; }
clocksrc_sync_source_t clocksrc_sync_source(void) { return s_sync_source; }
uint64_t clocksrc_last_sync(void) { return s_last_sync; }

uint64_t clocksrc_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec;
}

esp_err_t clocksrc_set_time(uint64_t unix_time)
{
    struct timeval tv = {.tv_sec = (time_t)unix_time};
    settimeofday(&tv, NULL);
    s_time_valid = true;
    s_sync_source = CLOCKSRC_SYNC_HOST;
    s_last_sync = unix_time;

    if (s_rtc != NULL) {
        esp_err_t err = rtc_put(unix_time);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RTC write failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
