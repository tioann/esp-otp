// Target-only command dispatch: maps protocol requests onto the clock and
// secret store. Pure framing stays in frame.c (host-testable); this file
// pulls in store/clocksrc and so only builds for the device.
#include <string.h>

#include "button.h"
#include "clocksrc.h"
#include "display.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "protocol.h"
#include "store.h"
#include "totp.h"

// Registered by the BLE layer; applies + persists / reads the advertised name.
static protocol_set_name_fn s_set_name_cb;
static protocol_get_name_fn s_get_name_cb;

void protocol_register_set_name(protocol_set_name_fn fn) { s_set_name_cb = fn; }
void protocol_register_get_name(protocol_get_name_fn fn) { s_get_name_cb = fn; }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// Single-byte status response.
static size_t status_only(uint8_t *resp, uint8_t status)
{
    resp[0] = status;
    return 1;
}

static size_t do_get_info(uint8_t *resp)
{
    uint8_t flags = 0;
    if (clocksrc_rtc_present()) flags |= 0x01;
    if (clocksrc_time_valid()) flags |= 0x02;
    resp[0] = ST_OK;
    resp[1] = OTP_FW_MAJOR;
    resp[2] = OTP_FW_MINOR;
    resp[3] = OTP_FW_PATCH;
    resp[4] = flags;
    wr16(&resp[5], (uint16_t)store_count());
    wr16(&resp[7], (uint16_t)store_capacity());
    // Trailing name_len(u8) + name(u8[]): appended after the fixed 9-byte header
    // so old clients that stop at byte 9 still parse. name_len <= DEVICE_NAME_MAX
    // (20) keeps the whole reply <= ~30 bytes, which fits the negotiated ATT MTU
    // (256) with room to spare — so it always rides along, no MTU gating needed.
    const char *name = s_get_name_cb ? s_get_name_cb() : NULL;
    size_t nlen = name ? strlen(name) : 0;
    if (nlen > PROTOCOL_DEVICE_NAME_MAX) nlen = PROTOCOL_DEVICE_NAME_MAX;
    resp[9] = (uint8_t)nlen;
    if (nlen) memcpy(&resp[10], name, nlen);
    return 10 + nlen;
}

static size_t do_set_time(const protocol_frame_t *req, uint8_t *resp)
{
    if (req->len < 4) {
        return status_only(resp, ST_BAD_REQUEST);
    }
    clocksrc_set_time(rd32(req->payload));
    return status_only(resp, ST_OK);
}

// LIST: pack as many records as fit, starting at index `start`.
static size_t do_list(const protocol_frame_t *req, uint8_t *resp, size_t resp_cap)
{
    size_t start = (req->len >= 1) ? req->payload[0] : 0;
    size_t total = store_count();

    resp[0] = ST_OK;
    wr16(&resp[1], (uint16_t)total);
    size_t pos = 4;       // leave resp[3] for the per-message entry count
    uint8_t n = 0;

    for (size_t i = start; i < total; i++) {
        store_record_t rec;
        if (!store_get(i, &rec)) {
            break;
        }
        size_t label_len = strlen(rec.label);
        size_t need = 2 + 1 + 2 + 1 + 1 + label_len;  // id,digits,period,algo,len,label
        if (pos + need > resp_cap) {
            break;
        }
        wr16(&resp[pos], rec.id); pos += 2;
        resp[pos++] = rec.digits;
        wr16(&resp[pos], rec.period); pos += 2;
        resp[pos++] = (uint8_t)rec.algo;
        resp[pos++] = (uint8_t)label_len;
        memcpy(&resp[pos], rec.label, label_len); pos += label_len;
        n++;
    }
    resp[3] = n;
    return pos;
}

static size_t do_add(const protocol_frame_t *req, uint8_t *resp)
{
    const uint8_t *p = req->payload;
    size_t len = req->len;
    // Fixed header: digits(1) period(2) algo(1) label_len(1)
    if (len < 5) {
        return status_only(resp, ST_BAD_REQUEST);
    }
    uint8_t digits = p[0];
    uint16_t period = rd16(&p[1]);
    uint8_t algo = p[3];
    uint8_t label_len = p[4];
    size_t off = 5;
    if (off + label_len + 1 > len) {
        return status_only(resp, ST_BAD_REQUEST);
    }
    const char *label = (const char *)&p[off];
    off += label_len;
    uint8_t secret_b32_len = p[off++];
    if (off + secret_b32_len > len) {
        return status_only(resp, ST_BAD_REQUEST);
    }

    char label_buf[STORE_LABEL_MAX + 1];
    if (label_len > STORE_LABEL_MAX) {
        return status_only(resp, ST_INVALID_ARG);
    }
    memcpy(label_buf, label, label_len);
    label_buf[label_len] = '\0';

    char b32[STORE_SECRET_MAX * 2 + 1];
    if (secret_b32_len >= sizeof(b32)) {
        return status_only(resp, ST_INVALID_ARG);
    }
    memcpy(b32, &p[off], secret_b32_len);
    b32[secret_b32_len] = '\0';

    uint8_t secret[STORE_SECRET_MAX];
    size_t secret_len = 0;
    if (base32_decode(b32, secret, sizeof(secret), &secret_len) != ESP_OK || secret_len == 0) {
        return status_only(resp, ST_INVALID_ARG);
    }

    uint16_t id = 0;
    esp_err_t err = store_add(label_buf, secret, secret_len, digits, period,
                              (totp_algo_t)algo, &id);
    if (err == ESP_ERR_NO_MEM) {
        return status_only(resp, ST_FULL);
    }
    if (err != ESP_OK) {
        return status_only(resp, ST_INVALID_ARG);
    }
    resp[0] = ST_OK;
    wr16(&resp[1], id);
    return 3;
}

static size_t do_remove(const protocol_frame_t *req, uint8_t *resp)
{
    if (req->len < 2) {
        return status_only(resp, ST_BAD_REQUEST);
    }
    esp_err_t err = store_remove(rd16(req->payload));
    return status_only(resp, err == ESP_OK ? ST_OK :
                             err == ESP_ERR_NOT_FOUND ? ST_NOT_FOUND : ST_INVALID_ARG);
}

static size_t do_rename(const protocol_frame_t *req, uint8_t *resp)
{
    if (req->len < 3) {
        return status_only(resp, ST_BAD_REQUEST);
    }
    uint16_t id = rd16(req->payload);
    uint8_t label_len = req->payload[2];
    if (3 + (size_t)label_len > req->len || label_len > STORE_LABEL_MAX) {
        return status_only(resp, ST_BAD_REQUEST);
    }
    char label[STORE_LABEL_MAX + 1];
    memcpy(label, &req->payload[3], label_len);
    label[label_len] = '\0';

    esp_err_t err = store_rename(id, label);
    return status_only(resp, err == ESP_OK ? ST_OK :
                             err == ESP_ERR_NOT_FOUND ? ST_NOT_FOUND : ST_INVALID_ARG);
}

// MOVE: reposition the token with `id` to cycle index `new_index`, shifting the
// intervening records to close the gap. Mutating, so auth-gated.
static size_t do_move(const protocol_frame_t *req, uint8_t *resp)
{
    if (req->len < 3) {
        return status_only(resp, ST_BAD_REQUEST);
    }
    uint16_t id = rd16(req->payload);
    uint8_t new_index = req->payload[2];
    esp_err_t err = store_move(id, new_index);
    return status_only(resp, err == ESP_OK ? ST_OK :
                             err == ESP_ERR_NOT_FOUND ? ST_NOT_FOUND : ST_INVALID_ARG);
}

// SET_NAME: change the BLE advertised device name so several people sharing a
// device can tell theirs apart. Payload is `name_len(1), name(u8[])` (UTF-8, not
// NUL-terminated). Mutating + identity-revealing, so auth-gated. The actual apply
// is delegated to the BLE layer via a registered hook (dependency inversion).
static size_t do_set_name(const protocol_frame_t *req, uint8_t *resp)
{
    if (req->len < 1) {
        return status_only(resp, ST_BAD_REQUEST);
    }
    uint8_t name_len = req->payload[0];
    if (1 + (size_t)name_len > req->len) {
        return status_only(resp, ST_BAD_REQUEST);
    }
    if (name_len == 0 || name_len > PROTOCOL_DEVICE_NAME_MAX) {
        return status_only(resp, ST_INVALID_ARG);
    }
    if (s_set_name_cb == NULL) {
        return status_only(resp, ST_UNSUPPORTED);
    }
    char name[PROTOCOL_DEVICE_NAME_MAX + 1];
    memcpy(name, &req->payload[1], name_len);
    name[name_len] = '\0';
    return status_only(resp, s_set_name_cb(name) ? ST_OK : ST_INVALID_ARG);
}

// GET_SCREEN: return a slice of the OLED framebuffer starting at byte `start`,
// packing as many bytes as fit `resp_cap`. The whole panel (width*pages bytes)
// won't fit one BLE MTU, so the client repeats with an advanced `start` until it
// has `total` bytes — same pagination pattern as LIST.
static size_t do_get_screen(const protocol_frame_t *req, uint8_t *resp, size_t resp_cap)
{
    const size_t header = 5;  // status + total(2) + width(1) + pages(1)
    if (resp_cap < header) {
        return status_only(resp, ST_BAD_REQUEST);
    }
    int width = 0, pages = 0;
    const uint8_t *fb = display_framebuffer(&width, &pages);
    size_t total = (size_t)width * (size_t)pages;
    size_t start = (req->len >= 2) ? rd16(req->payload) : 0;

    resp[0] = ST_OK;
    wr16(&resp[1], (uint16_t)total);
    resp[3] = (uint8_t)width;
    resp[4] = (uint8_t)pages;
    size_t pos = header;
    if (start < total) {
        size_t room = resp_cap - header;
        size_t n = total - start;
        if (n > room) n = room;
        memcpy(&resp[pos], fb + start, n);
        pos += n;
    }
    return pos;
}

// INJECT_BUTTON: drive the UI as if the physical button fired. payload[0] is the
// gesture (1=single, 2=double, 3=long); the event is posted to the same queue the
// main loop reads, so it cycles views / types codes / opens pairing just like a
// real press. Auth-gated: it can open the pairing window and trigger HID typing.
static size_t do_inject_button(const protocol_frame_t *req, uint8_t *resp)
{
    if (req->len < 1) {
        return status_only(resp, ST_BAD_REQUEST);
    }
    button_event_t ev;
    switch (req->payload[0]) {
        case OTP_BTN_SINGLE: ev = BUTTON_EVENT_SINGLE; break;
        case OTP_BTN_DOUBLE: ev = BUTTON_EVENT_DOUBLE; break;
        case OTP_BTN_LONG:   ev = BUTTON_EVENT_LONG;   break;
        default:             return status_only(resp, ST_INVALID_ARG);
    }
    esp_err_t err = button_inject(ev);
    return status_only(resp, err == ESP_OK ? ST_OK :
                             err == ESP_ERR_INVALID_ARG ? ST_INVALID_ARG : ST_BAD_REQUEST);
}

// REBOOT: restart the device. We must answer first, so the restart is deferred
// with a one-shot timer (~200 ms) — long enough for the response frame to reach
// the host over serial/BLE before the CPU resets. Auth-gated.
static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static size_t do_reboot(uint8_t *resp)
{
    static esp_timer_handle_t timer = NULL;
    if (timer == NULL) {
        const esp_timer_create_args_t args = {.callback = reboot_cb, .name = "reboot"};
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            return status_only(resp, ST_BAD_REQUEST);
        }
    }
    esp_timer_start_once(timer, 200 * 1000);  // 200 ms
    return status_only(resp, ST_OK);
}

// Commands that require an authorized link (bonded BLE / physical USB): the
// mutating ops, LIST (its labels leak which accounts are stored), SET_TIME (so
// only a paired host can steer the clock, which TOTP depends on), and GET_SCREEN
// (the panel can be showing a live TOTP code or the pairing passkey). Over BLE
// this means an encrypted, authenticated bond; over USB physical access counts.
// Only PING/GET_INFO stay open, so a fresh host can still probe the device.
static bool requires_auth(uint8_t op)
{
    return op == OP_ADD || op == OP_REMOVE || op == OP_RENAME ||
           op == OP_MOVE || op == OP_LIST || op == OP_SET_TIME ||
           op == OP_GET_SCREEN || op == OP_INJECT_BUTTON || op == OP_REBOOT ||
           op == OP_SET_NAME;
}

size_t protocol_dispatch(const protocol_frame_t *req, bool authorized,
                         uint8_t *resp, size_t resp_cap)
{
    if (resp_cap < 1) {
        return 0;
    }
    if (requires_auth(req->type) && !authorized) {
        return status_only(resp, ST_NOT_AUTHORIZED);
    }

    switch (req->type) {
        case OP_PING:     return status_only(resp, ST_OK);
        case OP_GET_INFO: return do_get_info(resp);
        case OP_SET_TIME: return do_set_time(req, resp);
        case OP_LIST:     return do_list(req, resp, resp_cap);
        case OP_ADD:      return do_add(req, resp);
        case OP_REMOVE:   return do_remove(req, resp);
        case OP_RENAME:   return do_rename(req, resp);
        case OP_MOVE:     return do_move(req, resp);
        case OP_SET_NAME: return do_set_name(req, resp);
        case OP_GET_SCREEN: return do_get_screen(req, resp, resp_cap);
        case OP_INJECT_BUTTON: return do_inject_button(req, resp);
        case OP_REBOOT:   return do_reboot(resp);
        default:          return status_only(resp, ST_UNSUPPORTED);
    }
}
