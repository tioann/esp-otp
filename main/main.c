// esp-otp — hardware TOTP token firmware entry point.
//
// Brings up storage, the shared I2C bus, the optional DS3231 clock source,
// the SSD1306 display, the push button and the USB management channel, then
// runs the code-view UI: it shows the current secret's TOTP code, cycles on a
// single press, and types the current code over BLE on a long-press.

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "blecon.h"
#include "button.h"
#include "clocksrc.h"
#include "display.h"
#include "driver/i2c_master.h"
#include "led.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "store.h"
#include "totp.h"
#include "usbcon.h"

static const char *TAG = "esp-otp";

// UI refresh cadence (also the marquee animation frame interval). The marquee
// speed/pauses are time-based (see canvas.h), independent of this.
#define OTP_REFRESH_MS       100

static i2c_master_bus_handle_t init_i2c(void)
{
    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .sda_io_num = CONFIG_OTP_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_OTP_I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    ESP_LOGI(TAG, "I2C up: SDA=%d SCL=%d", CONFIG_OTP_I2C_SDA_GPIO, CONFIG_OTP_I2C_SCL_GPIO);
    return bus;
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// Home/status screen (view 0): app name + entry count, the live date and
// time, and the last-sync source/timestamp. Re-rendered every refresh tick so
// the clock ticks in real time.
static void render_status(void)
{
    display_clear();
    char buf[20];
    struct tm tm;

    // Pages 0-1: current date (YYYY/MM/DD) and time (hh:mm:ss), live.
    if (clocksrc_time_valid()) {
        time_t now = (time_t)clocksrc_now();
        gmtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%Y/%m/%d", &tm);
        display_text(0, 0, buf);
        strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        display_text(0, 1, buf);
    } else {
        display_text(0, 0, "no time");
        display_text(0, 1, "yet");
    }

    // Page 3 is intentionally left blank: the sync freshness that used to be
    // shown here ("App: Nd ago") is now indicated by the status LED (steady =
    // synced within a day, pulsing = no time and no RTC). See components/led.

    // Page 4: number of stored OTP entries.
    snprintf(buf, sizeof(buf), "entries %u", (unsigned)store_count());
    display_text(0, 4, buf);

    // Page 2: active HID keystroke target (double-click here cycles it). A '+'
    // suffix flags that more than one keyboard host is connected to pick from.
    char hid[12];
    size_t hosts = blecon_hid_target_label(hid, sizeof(hid));
    snprintf(buf, sizeof(buf), "HID %s%s", hid, hosts > 1 ? "+" : "");
    display_text(0, 2, buf);

    display_flush();
}

// Pairing screen: the 6-digit passkey the user must type on the connecting
// host (legacy fallback). Overrides the normal views while a pairing is in
// progress.
static void render_passkey(uint32_t passkey)
{
    display_clear();
    display_text(0, 0, "Pair code:");
    char buf[8];
    snprintf(buf, sizeof(buf), "%06u", (unsigned)passkey);
    display_text_big(0, 2, buf);  // 6 glyphs x 12px = 72px, full panel width
    display_flush();
}

// Numeric-comparison screen: the same 6-digit code is shown on the phone. The
// user checks they match and single-presses the button to confirm.
static void render_numcmp(uint32_t code)
{
    display_clear();
    display_text(0, 0, "Match? press");
    char buf[8];
    snprintf(buf, sizeof(buf), "%06u", (unsigned)code);
    display_text_big(0, 2, buf);  // 6 glyphs x 12px = 72px, full panel width
    display_flush();
}

// Shown after the pairing window opens, until a host connects and a passkey
// appears (or the window times out).
static void render_pairing_wait(void)
{
    display_clear();
    display_text(0, 1, "Pairing...");
    display_text(0, 3, "connect now");
    display_flush();
}

// `elapsed_ms` is how long the current item has been shown; it drives the
// label marquee and is reset by the caller when the item changes.
static void render_entry(size_t index, uint32_t elapsed_ms)
{
    display_clear();

    store_record_t rec;
    if (!store_get(index, &rec)) {
        display_flush();
        return;
    }

    // Without a valid time the code is meaningless: show dashes instead.
    uint64_t now = clocksrc_now();
    bool have_time = clocksrc_time_valid();
    char codebuf[12];
    if (have_time) {
        uint32_t code = totp_compute(rec.secret, rec.secret_len, now, rec.period,
                                     rec.digits, rec.algo);
        snprintf(codebuf, sizeof(codebuf), "%0*u", rec.digits, (unsigned)code);
    } else {
        memset(codebuf, '-', rec.digits);
        codebuf[rec.digits < sizeof(codebuf) ? rec.digits : sizeof(codebuf) - 1] = '\0';
    }

    // Header (page 0): the current position "n" sits at the right; the label
    // fills the rest and scrolls (marquee) when it is too long to fit.
    char pos[12];
    snprintf(pos, sizeof(pos), "%u", (unsigned)(index + 1));
    int pos_px = (int)strlen(pos) * DISPLAY_FONT_WIDTH;
    int label_px = DISPLAY_WIDTH - pos_px - DISPLAY_FONT_WIDTH;  // 1-char gap
    display_marquee(0, label_px, 0, rec.label, elapsed_ms);
    display_text(DISPLAY_WIDTH - pos_px, 0, pos);

    // Code on pages 2-3: big (2x) when it fits the 72px width, otherwise 1x.
    int big_px = (int)strlen(codebuf) * DISPLAY_FONT_WIDTH * 2;
    if (big_px <= DISPLAY_WIDTH) {
        display_text_big((DISPLAY_WIDTH - big_px) / 2, 2, codebuf);
    } else {
        int small_px = (int)strlen(codebuf) * DISPLAY_FONT_WIDTH;
        display_text((DISPLAY_WIDTH - small_px) / 2, 2, codebuf);
    }

    // Remaining-seconds progress bar on the last page (only with a valid time).
    if (have_time) {
        uint32_t period = rec.period ? rec.period : 30;
        uint32_t remaining = period - (uint32_t)(now % period);
        int width = (int)((remaining * DISPLAY_WIDTH) / period);
        display_hbar(0, DISPLAY_PAGES - 1, width, 0x3C);
    }

    display_flush();
}

void app_main(void)
{
    ESP_LOGI(TAG, "esp-otp booting");

    init_nvs();
    ESP_ERROR_CHECK(store_init());

    QueueHandle_t buttons = xQueueCreate(8, sizeof(button_event_t));
    ESP_ERROR_CHECK(buttons ? ESP_OK : ESP_ERR_NO_MEM);

    i2c_master_bus_handle_t bus = init_i2c();
    ESP_ERROR_CHECK(display_init(bus));
    ESP_ERROR_CHECK(clocksrc_init(bus));

    // Status LED reflects clock-sync state; reads clocksrc, so init it after.
    // No-op when disabled.
    ESP_ERROR_CHECK(led_init());

#if CONFIG_OTP_DEMO_MODE
    // Bring-up aid: seed a known secret and time so a live code renders without
    // a host. The secret is the RFC 6238 SHA1 test seed.
    if (store_count() == 0) {
        // label, base32 secret, digits, period, algo — mix of types so the
        // cycle/render paths get exercised (6 vs 8 digits, SHA1/256/512).
        static const struct {
            const char *label;
            const char *b32;
            uint8_t digits;
            uint16_t period;
            totp_algo_t algo;
        } seeds[] = {
            {"Work-Email-2FA", "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", 6, 30, TOTP_SHA1},
            {"GitHub",         "JBSWY3DPEHPK3PXP",                 6, 30, TOTP_SHA256},
            {"AWS-root",       "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", 8, 30, TOTP_SHA512},
        };
        for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
            uint8_t secret[STORE_SECRET_MAX];
            size_t slen = 0;
            if (base32_decode(seeds[i].b32, secret, sizeof(secret), &slen) == ESP_OK) {
                uint16_t id;
                store_add(seeds[i].label, secret, slen, seeds[i].digits,
                          seeds[i].period, seeds[i].algo, &id);
            }
        }
    }
    if (!clocksrc_time_valid()) {
        clocksrc_set_time(1700000000);  // fixed epoch; advances from here
    }
#endif

    ESP_ERROR_CHECK(button_init(CONFIG_OTP_BUTTON_GPIO, buttons));

    // USB-Serial/JTAG management channel.
    ESP_ERROR_CHECK(usbcon_init());

    // BLE: management GATT (sync time / manage secrets) + HID keyboard for the
    // long-press "type the code" feature.
    ESP_ERROR_CHECK(blecon_init());

    // View 0 is the status/home screen; views 1..count map to entries 0..count-1.
    // A single press advances through the entries and wraps back to the status
    // screen after the last one.
    size_t view = 0;
    int64_t shown_since_us = esp_timer_get_time();  // when the current view appeared

    for (;;) {
        // Short wait so the marquee animates smoothly; also refreshes the code
        // and progress bar. Returns early on a button press.
        button_event_t ev;
        if (xQueueReceive(buttons, &ev, pdMS_TO_TICKS(OTP_REFRESH_MS)) == pdTRUE) {
            size_t count = store_count();
            if (blecon_pairing_confirm_pending(NULL)) {
                // A numeric-comparison confirm is on screen: the button answers
                // it (single = codes match, long = reject) instead of driving
                // the normal views.
                if (ev == BUTTON_EVENT_SINGLE) {
                    blecon_confirm_pairing(true);
                } else if (ev == BUTTON_EVENT_LONG) {
                    blecon_confirm_pairing(false);
                }
            } else if (ev == BUTTON_EVENT_SINGLE) {
                view = (view + 1) % (count + 1);  // 0=status, 1..count=entries
                shown_since_us = esp_timer_get_time();  // restart marquee for new view
            } else if (ev == BUTTON_EVENT_LONG && view == 0) {
                // On the home screen a long-press opens the BLE pairing window so
                // a new host can bond (shows a passkey on the OLED).
                blecon_open_pairing();
            } else if (ev == BUTTON_EVENT_LONG && view > 0) {
                // On an entry, a long-press types the current code over BLE HID
                // (digits only, no trailing Enter). Needs a valid time and a
                // subscribed host.
                store_record_t rec;
                if (clocksrc_time_valid() && store_get(view - 1, &rec)) {
                    uint32_t code = totp_compute(rec.secret, rec.secret_len,
                                                 clocksrc_now(), rec.period,
                                                 rec.digits, rec.algo);
                    char codebuf[12];
                    snprintf(codebuf, sizeof(codebuf), "%0*u", rec.digits, (unsigned)code);
                    esp_err_t err = blecon_type_code(codebuf);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "type-over-BLE skipped: %s", esp_err_to_name(err));
                    }
                }
            } else if (ev == BUTTON_EVENT_DOUBLE && view == 0) {
                // On the home screen a double-click cycles the HID keystroke
                // target between connected keyboard hosts (laptop/phone/...).
                blecon_cycle_hid_target();
            }
        }
        if (view > store_count()) {  // entries removed while viewed: fall back home
            view = 0;
        }
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - shown_since_us) / 1000);
        uint32_t passkey;
        if (blecon_pairing_confirm_pending(&passkey)) {
            render_numcmp(passkey);       // codes shown both sides: confirm match
        } else if (blecon_pairing_passkey(&passkey)) {
            render_passkey(passkey);      // legacy: host types the code
        } else if (blecon_pairing_open()) {
            render_pairing_wait();        // window open, waiting for a host
        } else if (view == 0) {
            render_status();
        } else {
            render_entry(view - 1, elapsed_ms);
        }
    }
}
