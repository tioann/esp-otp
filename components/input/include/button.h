// Single push button with debounce and single/double/long-press detection.
//
// A GPIO (active-low, internal pull-up) is debounced in software; click
// gestures are classified by a short timer and delivered as events on a
// FreeRTOS queue so the main loop can stay simple.
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_EVENT_SINGLE = 1,  // one press-release -> cycle to next code
    BUTTON_EVENT_DOUBLE = 2,  // two quick presses -> type current code (BLE HID)
    BUTTON_EVENT_LONG   = 3,  // held ~1.5s -> open the BLE pairing window
} button_event_t;

// Initialise the button on `gpio`. Events are posted to `out_queue`, which
// the caller creates (items of type button_event_t). Returns ESP_OK on
// success.
esp_err_t button_init(int gpio, QueueHandle_t out_queue);

// Post a synthetic button event onto the queue passed to button_init(), as if
// the hardware button had generated it (used by the protocol INJECT_BUTTON
// command). Returns ESP_ERR_INVALID_STATE if button_init() hasn't run,
// ESP_ERR_INVALID_ARG for an out-of-range event.
esp_err_t button_inject(button_event_t ev);

#ifdef __cplusplus
}
#endif
