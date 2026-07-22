// Pure monochrome framebuffer + text drawing for the SSD1306, with no
// ESP-IDF dependencies so the exact same rendering can run on the host.
//
// Buffer layout matches the SSD1306: CANVAS_PAGES pages of CANVAS_WIDTH
// columns, each byte a vertical run of 8 pixels (bit0 = top row).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Physical panel is the 0.42" 72x40 OLED (the SSD1306 controller addresses a
// larger 128x64 GDDRAM, but only this sub-window is wired — see ssd1306.c for
// the column/page offset applied on flush).
#define CANVAS_WIDTH       72
#define CANVAS_PAGES       5     // 5 pages * 8 rows = 40 px tall
#define CANVAS_FONT_WIDTH  6
#define CANVAS_FONT_HEIGHT 8

void canvas_clear(void);
void canvas_text(int x, int page, const char *s);       // 6x8
void canvas_text_big(int x, int page, const char *s);   // 12x16, spans 2 pages
void canvas_hbar(int x, int page, int width, uint8_t mask);

// Marquee timing.
#define CANVAS_MARQUEE_PAUSE_MS   1000  // hold at each end
#define CANVAS_MARQUEE_SPEED_PXS  20    // scroll speed, pixels per second

// Draw `s` within the horizontal window [x0, x1), clipped to it. If the text
// fits, it is drawn static at x0. Otherwise it ping-pongs as a function of
// `elapsed_ms`: hold at the start for CANVAS_MARQUEE_PAUSE_MS, scroll left until
// the last character is fully shown, hold again, scroll back, and repeat.
void canvas_marquee(int x0, int x1, int page, const char *s, uint32_t elapsed_ms);

// Read-only view of the framebuffer (CANVAS_PAGES * CANVAS_WIDTH bytes).
const uint8_t *canvas_buffer(void);

#ifdef __cplusplus
}
#endif
