// Minimal SSD1306 text display for esp-otp (0.42" 72x40 panel, I2C).
//
// Self-contained driver on the new i2c_master API: it owns a 1 KB page
// framebuffer and a 6x8 font, so no external graphics/font dependency is
// fetched at build time (keeps the Docker build hermetic). The I2C bus is
// created by the caller and shared with the optional DS3231 RTC.
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 0.42" 72x40 panel. The SSD1306 controller has 128x64 of RAM but only a
// 72x40 window is physically connected, offset within that address space.
#define DISPLAY_WIDTH   72
#define DISPLAY_PAGES   5     // 5 pages of 8 vertical pixels = 40 rows
#define DISPLAY_I2C_ADDR 0x3C
#define DISPLAY_FONT_WIDTH  6 // 6x8 font cell; big text is 2x this
#define DISPLAY_FONT_HEIGHT 8

// Visible-area offsets inside the controller's GDDRAM. 28/0 are the usual
// values for the 0.42" 72x40 module; tweak if the image is shifted.
#define DISPLAY_COL_OFFSET  28
#define DISPLAY_PAGE_OFFSET 0

// Attach the SSD1306 at DISPLAY_I2C_ADDR on an existing master bus and
// initialise the panel. Returns ESP_OK on success.
esp_err_t display_init(i2c_master_bus_handle_t bus);

// Read-only snapshot of the current framebuffer, `*width` columns x `*pages`
// byte-rows (each byte a vertical run of 8 pixels, bit0 = top). Backs the
// GET_SCREEN command so a host can fetch exactly what the OLED shows.
const uint8_t *display_framebuffer(int *width, int *pages);

// Clear the in-memory framebuffer (call display_flush to push it).
void display_clear(void);

// Draw a 6x8 string at column `x` (pixels) and `page` (0..7). Characters
// outside the font range render as blanks; drawing is clipped to the panel.
void display_text(int x, int page, const char *s);

// Draw a 2x-scaled string (12x16 per glyph) spanning two pages starting at
// `page`. Used for the big TOTP digits.
void display_text_big(int x, int page, const char *s);

// Draw `s` within [x0, x1), ping-pong scrolling (marquee) when it doesn't fit.
// `elapsed_ms` is the time the item has been displayed; the caller resets it
// when the shown item changes.
void display_marquee(int x0, int x1, int page, const char *s, uint32_t elapsed_ms);

// Fill a horizontal bar of `width` pixels on `page`, rows masked by `mask`
// (e.g. 0xFF = full page height). Used for the seconds progress bar.
void display_hbar(int x, int page, int width, uint8_t mask);

// Push the framebuffer to the panel.
esp_err_t display_flush(void);

#ifdef __cplusplus
}
#endif
