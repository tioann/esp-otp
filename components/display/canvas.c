#include "canvas.h"

#include <string.h>

#include "font6x8.h"

static uint8_t s_fb[CANVAS_PAGES][CANVAS_WIDTH];

void canvas_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

const uint8_t *canvas_buffer(void)
{
    return &s_fb[0][0];
}

// Return the 6 column bytes for a character, or zeros if out of range.
static const uint8_t *glyph(char c)
{
    static const uint8_t blank[FONT_WIDTH] = {0};
    unsigned uc = (unsigned char)c;
    if (uc < FONT_FIRST_CHAR || uc > FONT_LAST_CHAR) {
        return blank;
    }
    return font6x8[uc - FONT_FIRST_CHAR];
}

void canvas_text(int x, int page, const char *s)
{
    if (page < 0 || page >= CANVAS_PAGES) {
        return;
    }
    for (; *s; s++) {
        const uint8_t *g = glyph(*s);
        for (int col = 0; col < FONT_WIDTH; col++, x++) {
            if (x >= 0 && x < CANVAS_WIDTH) {
                s_fb[page][x] = g[col];
            }
        }
    }
}

// Expand the low nibble of `b` into a byte by doubling each bit vertically.
static uint8_t expand_nibble(uint8_t b)
{
    uint8_t out = 0;
    for (int i = 0; i < 4; i++) {
        if (b & (1 << i)) {
            out |= 0x03 << (i * 2);
        }
    }
    return out;
}

void canvas_text_big(int x, int page, const char *s)
{
    if (page < 0 || page + 1 >= CANVAS_PAGES) {
        return;
    }
    for (; *s; s++) {
        const uint8_t *g = glyph(*s);
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint8_t lo = expand_nibble(g[col] & 0x0F);        // top 4 rows -> page
            uint8_t hi = expand_nibble((g[col] >> 4) & 0x0F); // bottom 4 rows -> page+1
            for (int dup = 0; dup < 2; dup++, x++) {          // 2x horizontal
                if (x >= 0 && x < CANVAS_WIDTH) {
                    s_fb[page][x] = lo;
                    s_fb[page + 1][x] = hi;
                }
            }
        }
    }
}

void canvas_hbar(int x, int page, int width, uint8_t mask)
{
    if (page < 0 || page >= CANVAS_PAGES) {
        return;
    }
    for (int i = 0; i < width; i++, x++) {
        if (x >= 0 && x < CANVAS_WIDTH) {
            s_fb[page][x] = mask;
        }
    }
}

// Draw a string starting at (x0 - shift), clipped to the window [x0, x1).
static void canvas_text_window(int x0, int x1, int page, const char *s, int shift)
{
    if (page < 0 || page >= CANVAS_PAGES) {
        return;
    }
    int x = x0 - shift;
    for (; *s; s++) {
        const uint8_t *g = glyph(*s);
        for (int col = 0; col < FONT_WIDTH; col++, x++) {
            if (x >= x0 && x < x1 && x >= 0 && x < CANVAS_WIDTH) {
                s_fb[page][x] = g[col];
            }
        }
    }
}

void canvas_marquee(int x0, int x1, int page, const char *s, uint32_t elapsed_ms)
{
    int win = x1 - x0;
    int text_px = (int)strlen(s) * FONT_WIDTH;
    int max_shift = text_px - win;
    if (max_shift <= 0) {
        canvas_text_window(x0, x1, page, s, 0);  // fits: no scroll
        return;
    }

    // One ping-pong cycle: pause, scroll out, pause, scroll back.
    int travel_ms = max_shift * 1000 / CANVAS_MARQUEE_SPEED_PXS;
    int pause_ms = CANVAS_MARQUEE_PAUSE_MS;
    int cycle = 2 * pause_ms + 2 * travel_ms;
    int t = (int)(elapsed_ms % (uint32_t)cycle);

    int shift;
    if (t < pause_ms) {
        shift = 0;                                            // hold at start
    } else if (t < pause_ms + travel_ms) {
        shift = (t - pause_ms) * max_shift / travel_ms;       // scroll forward
    } else if (t < 2 * pause_ms + travel_ms) {
        shift = max_shift;                                    // hold at end
    } else {
        shift = max_shift - (t - 2 * pause_ms - travel_ms) * max_shift / travel_ms;  // back
    }
    canvas_text_window(x0, x1, page, s, shift);
}
