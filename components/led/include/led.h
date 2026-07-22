// Status LED driver (the C3 super mini's onboard blue LED on GPIO8).
//
// A background task reflects the clock-sync state on the LED:
//   - STEADY at a configurable brightness when the clock was synced less than
//     a day ago (from a healthy RTC at boot, or a host push).
//   - PULSE (breathing) otherwise — no valid time, a dead RTC, or a sync older
//     than a day — flagging that the time can't be trusted.
//
// GPIO, active-low polarity and steady brightness are Kconfig options. When
// disabled (CONFIG_OTP_LED_ENABLE=n) led_init() is a no-op.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configure the LED (LEDC PWM) and start the status task. Reads clock state via
// clocksrc, so call after clocksrc_init(). Safe to call once.
esp_err_t led_init(void);

#ifdef __cplusplus
}
#endif
