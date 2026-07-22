#include "display.h"

#include <string.h>

#include "canvas.h"
#include "esp_log.h"

static const char *TAG = "display";

#define SSD1306_CMD  0x00
#define SSD1306_DATA 0x40

static i2c_master_dev_handle_t s_dev;

const uint8_t *display_framebuffer(int *width, int *pages)
{
    if (width) *width = CANVAS_WIDTH;
    if (pages) *pages = CANVAS_PAGES;
    return canvas_buffer();
}

static esp_err_t send_cmds(const uint8_t *cmds, size_t n)
{
    // Each command is prefixed with a control byte; send them one by one to
    // keep the transfer buffers small and the sequence easy to read.
    for (size_t i = 0; i < n; i++) {
        uint8_t buf[2] = {SSD1306_CMD, cmds[i]};
        esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), 1000);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t display_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DISPLAY_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device: %s", esp_err_to_name(err));
        return err;
    }

    static const uint8_t init_seq[] = {
        0xAE,             // display off
        0xD5, 0x80,       // clock div
        0xA8, 0x27,       // multiplex = 39 (40 rows)
        0xD3, 0x00,       // display offset
        0x40,             // start line 0
        0x8D, 0x14,       // charge pump on
        0x20, 0x00,       // horizontal addressing mode
        0xA1,             // segment remap
        0xC8,             // COM scan direction reversed
        0xDA, 0x12,       // COM pins
        0x81, 0xCF,       // contrast
        0xD9, 0xF1,       // pre-charge
        0xDB, 0x40,       // VCOM detect
        0xA4,             // resume from RAM
        0xA6,             // normal (non-inverted)
        0x2E,             // scroll off
        0xAF,             // display on
    };
    err = send_cmds(init_seq, sizeof(init_seq));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init seq: %s", esp_err_to_name(err));
        return err;
    }

    canvas_clear();
    err = display_flush();
    ESP_LOGI(TAG, "SSD1306 ready");
    return err;
}

// Drawing is delegated to the shared (host-testable) canvas module.
void display_clear(void) { canvas_clear(); }
void display_text(int x, int page, const char *s) { canvas_text(x, page, s); }
void display_text_big(int x, int page, const char *s) { canvas_text_big(x, page, s); }
void display_hbar(int x, int page, int width, uint8_t mask) { canvas_hbar(x, page, width, mask); }
void display_marquee(int x0, int x1, int page, const char *s, uint32_t elapsed_ms) { canvas_marquee(x0, x1, page, s, elapsed_ms); }

esp_err_t display_flush(void)
{
    static const uint8_t window[] = {
        // Visible window is offset within the controller's GDDRAM.
        0x21, DISPLAY_COL_OFFSET, DISPLAY_COL_OFFSET + DISPLAY_WIDTH - 1,
        0x22, DISPLAY_PAGE_OFFSET, DISPLAY_PAGE_OFFSET + DISPLAY_PAGES - 1,
    };
    esp_err_t err = send_cmds(window, sizeof(window));
    if (err != ESP_OK) {
        return err;
    }

    // One control byte followed by the whole framebuffer.
    uint8_t buf[1 + CANVAS_PAGES * CANVAS_WIDTH];
    buf[0] = SSD1306_DATA;
    memcpy(&buf[1], canvas_buffer(), CANVAS_PAGES * CANVAS_WIDTH);
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 1000);
}
