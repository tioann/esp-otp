#include "led.h"

#include "sdkconfig.h"

#if CONFIG_OTP_LED_ENABLE

#include "clocksrc.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

// PWM setup. 10-bit resolution is plenty for an indicator LED; ~5 kHz is well
// above the flicker threshold and comfortably within the C3's LEDC limits.
#define LED_TIMER      LEDC_TIMER_0
#define LED_CHANNEL    LEDC_CHANNEL_0
#define LED_MODE       LEDC_LOW_SPEED_MODE   // the C3 only has the low-speed group
#define LED_RES        LEDC_TIMER_10_BIT
#define LED_DUTY_MAX   ((1 << 10) - 1)       // 1023
#define LED_FREQ_HZ    5000

// Animation: one tick per step; a full breathe cycle is BREATHE_STEPS ticks.
#define TICK_MS        20
#define BREATHE_STEPS  100                    // ~2 s up-and-down at 20 ms/tick

// Fraction of a day (seconds) within which a sync counts as "fresh".
#define SYNC_FRESH_SECS 86400ULL

// Map a logical 0..LED_DUTY_MAX brightness to the hardware duty, honouring the
// active-low wiring of the onboard LED (lit when the pin is driven LOW).
static void led_set(uint32_t brightness)
{
    if (brightness > LED_DUTY_MAX) {
        brightness = LED_DUTY_MAX;
    }
#if CONFIG_OTP_LED_ACTIVE_LOW
    uint32_t duty = LED_DUTY_MAX - brightness;
#else
    uint32_t duty = brightness;
#endif
    ledc_set_duty(LED_MODE, LED_CHANNEL, duty);
    ledc_update_duty(LED_MODE, LED_CHANNEL);
}

// Steady-on brightness from the configured intensity percent.
static uint32_t steady_brightness(void)
{
    return (uint32_t)LED_DUTY_MAX * CONFIG_OTP_LED_INTENSITY / 100;
}

typedef enum { LED_STEADY, LED_PULSE } led_state_t;

// Steady only when the clock was synced (from a healthy RTC at boot or a host
// push) within the last day. Everything else — no time, a dead RTC, or a sync
// older than a day — pulses to flag that the time can't be trusted.
static led_state_t classify(void)
{
    if (clocksrc_time_valid()) {
        uint64_t now = clocksrc_now();
        uint64_t last = clocksrc_last_sync();
        if (now >= last && (now - last) < SYNC_FRESH_SECS) {
            return LED_STEADY;
        }
    }
    return LED_PULSE;
}

static void led_task(void *arg)
{
    (void)arg;
    int step = 0;
    for (;;) {
        if (classify() == LED_STEADY) {
            led_set(steady_brightness());
            step = 0;
        } else {
            // Triangle breathe between 0 and the steady brightness so the pulse
            // peak matches the configured intensity.
            uint32_t peak = steady_brightness();
            int half = BREATHE_STEPS / 2;
            int up = step < half ? step : (BREATHE_STEPS - step);
            led_set(peak * up / half);
            step = (step + 1) % BREATHE_STEPS;
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t led_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LED_MODE,
        .timer_num = LED_TIMER,
        .duty_resolution = LED_RES,
        .freq_hz = LED_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t ch = {
        .gpio_num = CONFIG_OTP_LED_GPIO,
        .speed_mode = LED_MODE,
        .channel = LED_CHANNEL,
        .timer_sel = LED_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(err));
        return err;
    }
    led_set(0);  // start dark until the task decides otherwise

    if (xTaskCreate(led_task, "led", 2560, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "status LED on GPIO%d (active %s, %d%% steady)",
             CONFIG_OTP_LED_GPIO, CONFIG_OTP_LED_ACTIVE_LOW ? "low" : "high",
             CONFIG_OTP_LED_INTENSITY);
    return ESP_OK;
}

#else  // disabled or simulator

esp_err_t led_init(void) { return ESP_OK; }

#endif
