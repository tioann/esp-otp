#include "button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "button";

// Timing, in milliseconds.
#define POLL_MS        5
#define DEBOUNCE_MS    20
#define DOUBLE_GAP_MS  300
#define LONG_MS        1500  // hold this long -> BUTTON_EVENT_LONG

typedef struct {
    int gpio;
    QueueHandle_t queue;
} button_ctx_t;

// The queue registered by button_init(), so button_inject() can post synthetic
// events onto the same path the hardware task uses.
static QueueHandle_t s_event_queue = NULL;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Poll + debounce + click classification. A completed click is a clean
// pressed->released transition. The first click opens a window; a second
// click closing within DOUBLE_GAP_MS is a double, otherwise the first
// resolves to a single when the window expires.
static void button_task(void *arg)
{
    button_ctx_t *ctx = (button_ctx_t *)arg;

    int stable = 1;          // 1 == released (active-low + pull-up)
    int last_raw = 1;
    int64_t last_change = now_ms();
    bool click_pending = false;
    int64_t click_deadline = 0;
    int64_t press_start = 0;   // when the current press began (stable == 0)
    bool long_fired = false;   // LONG already delivered for this press

    for (;;) {
        int raw = gpio_get_level(ctx->gpio);
        int64_t t = now_ms();

        if (raw != last_raw) {
            last_raw = raw;
            last_change = t;
        }

        if (raw != stable && (t - last_change) >= DEBOUNCE_MS) {
            stable = raw;
            if (stable == 0) {  // pressed edge -> start timing for a long press
                press_start = t;
                long_fired = false;
            } else {            // released edge -> one completed click
                if (long_fired) {
                    // Long press already delivered; the release just ends it.
                } else if (click_pending) {
                    click_pending = false;
                    button_event_t ev = BUTTON_EVENT_DOUBLE;
                    xQueueSend(ctx->queue, &ev, 0);
                } else {
                    click_pending = true;
                    click_deadline = t + DOUBLE_GAP_MS;
                }
            }
        }

        // Fire LONG once while the button is still held past the threshold.
        if (stable == 0 && !long_fired && (t - press_start) >= LONG_MS) {
            long_fired = true;
            click_pending = false;  // a long press is not also a click
            button_event_t ev = BUTTON_EVENT_LONG;
            xQueueSend(ctx->queue, &ev, 0);
        }

        if (click_pending && t >= click_deadline) {
            click_pending = false;
            button_event_t ev = BUTTON_EVENT_SINGLE;
            xQueueSend(ctx->queue, &ev, 0);
        }

        // Always block for at least one tick: at a 100 Hz tick pdMS_TO_TICKS(5)
        // rounds to 0, which would busy-spin this priority-5 task and starve the
        // lower-priority UI loop.
        TickType_t delay = pdMS_TO_TICKS(POLL_MS);
        vTaskDelay(delay ? delay : 1);
    }
}

esp_err_t button_init(int gpio, QueueHandle_t out_queue)
{
    if (out_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    button_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->gpio = gpio;
    ctx->queue = out_queue;

    if (xTaskCreate(button_task, "button", 2560, ctx, 5, NULL) != pdPASS) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    s_event_queue = out_queue;
    ESP_LOGI(TAG, "button on GPIO%d", gpio);
    return ESP_OK;
}

esp_err_t button_inject(button_event_t ev)
{
    if (s_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ev != BUTTON_EVENT_SINGLE && ev != BUTTON_EVENT_DOUBLE && ev != BUTTON_EVENT_LONG) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "inject event %d", (int)ev);
    return xQueueSend(s_event_queue, &ev, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}
