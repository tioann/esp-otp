// Time source abstraction with an OPTIONAL DS3231 RTC.
//
// The system clock is always the single source of truth for TOTP. If a
// DS3231 is detected on the I2C bus and its oscillator is healthy, its time
// seeds the system clock at boot (so codes work immediately, no host needed).
// With no RTC — or an RTC whose battery died — time is "invalid" until a host
// pushes it via clocksrc_set_time(); the firmware otherwise runs identically.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Where the clock's current time most recently came from.
typedef enum {
    CLOCKSRC_SYNC_NONE = 0,  // never synced; time invalid
    CLOCKSRC_SYNC_RTC,       // seeded from a healthy DS3231 at boot
    CLOCKSRC_SYNC_HOST,      // pushed by the host / mobile app via set_time
} clocksrc_sync_source_t;

// Probe for a DS3231 on `bus` and, if present and running, seed the system
// clock from it. Always returns ESP_OK (absence of an RTC is not an error).
esp_err_t clocksrc_init(i2c_master_bus_handle_t bus);

// True if a DS3231 was detected at boot.
bool clocksrc_rtc_present(void);

// Source of the most recent successful sync (NONE until the first one).
clocksrc_sync_source_t clocksrc_sync_source(void);

// Unix time at the moment of the last sync (0 if never synced). For an RTC
// boot-seed this is the time read back; for a host push it is the time set.
uint64_t clocksrc_last_sync(void);

// True once the clock has a trustworthy time (seeded from a healthy RTC or
// set by a host since boot). The UI shows codes only when this is true.
bool clocksrc_time_valid(void);

// Current Unix time in seconds.
uint64_t clocksrc_now(void);

// Set the current time (Unix seconds): updates the system clock, marks time
// valid, and — if an RTC is present — writes it back and clears the
// oscillator-stop flag. This backs the SET_TIME protocol command.
esp_err_t clocksrc_set_time(uint64_t unix_time);

#ifdef __cplusplus
}
#endif
