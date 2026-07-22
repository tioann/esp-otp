#include "usbcon.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol.h"

static const char *TAG = "usbcon";

static void rx_task(void *arg)
{
    (void)arg;
    protocol_parser_t parser;
    protocol_parser_reset(&parser);

    for (;;) {
        uint8_t b;
        int n = usb_serial_jtag_read_bytes(&b, 1, portMAX_DELAY);
        if (n != 1) {
            continue;
        }

        protocol_frame_t req;
        if (!protocol_parser_push(&parser, b, &req)) {
            continue;
        }

        // USB = physical access => authorized for mutating commands.
        uint8_t resp[PROTOCOL_MAX_PAYLOAD];
        size_t rlen = protocol_dispatch(&req, true, resp, sizeof(resp));

        uint8_t frame[PROTOCOL_MAX_FRAME];
        size_t flen = protocol_encode(req.type | PROTOCOL_RESP_BIT, req.seq,
                                      resp, (uint16_t)rlen, frame, sizeof(frame));
        // write_bytes only enqueues what fits the TX ring buffer, so loop until
        // the whole frame is out — a full GET_SCREEN frame exceeds one bufferful.
        size_t sent = 0;
        while (sent < flen) {
            int w = usb_serial_jtag_write_bytes(frame + sent, flen - sent,
                                                pdMS_TO_TICKS(100));
            if (w <= 0) {
                break;  // host not draining; drop the rest rather than block forever
            }
            sent += (size_t)w;
        }
    }
}

esp_err_t usbcon_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    // The default 256-byte buffers are one byte short of a full frame (a
    // GET_SCREEN response or a max ADD request is up to PROTOCOL_MAX_FRAME=257),
    // which truncated large transfers. Give both room for a whole frame.
    cfg.tx_buffer_size = 512;
    cfg.rx_buffer_size = 512;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (xTaskCreate(rx_task, "usbcon", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB management channel up");
    return ESP_OK;
}
