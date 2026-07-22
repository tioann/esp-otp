// GATT services: the custom management service (framed wire protocol, shared
// with USB) plus a standard report-protocol HID keyboard and Device
// Information service. See PROTOCOL.md for the management wire format and
// ble_internal.h for the glue exposed to blecon.c.
#include "sdkconfig.h"

#include <string.h>

#include "ble_internal.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
#include "protocol.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_gatt";

// --- Management service UUIDs (128-bit, base 6f7470xx-0000-1000-8000-...) ---
// Byte order for BLE_UUID128_INIT is little-endian (reverse of the text form).
// Non-static: blecon.c advertises this UUID so scanners can identify an esp-otp
// device by type (rename-proof), not just by its changeable GAP name.
const ble_uuid128_t mgmt_svc_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x70, 0x74, 0x6f);
static const ble_uuid128_t mgmt_cmd_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x01, 0x70, 0x74, 0x6f);
static const ble_uuid128_t mgmt_resp_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x02, 0x70, 0x74, 0x6f);

// 16-bit SIG UUIDs for the HID and Device Information services.
#define UUID_DIS          0x180A
#define UUID_PNP_ID       0x2A50
#define UUID_MANUFACTURER 0x2A29
#define UUID_HID          0x1812
#define UUID_HID_INFO     0x2A4A
#define UUID_REPORT_MAP   0x2A4B
#define UUID_HID_CTRL     0x2A4C
#define UUID_REPORT       0x2A4D
#define UUID_REPORT_REF   0x2908

// Captured value handles for notifications (filled in at registration).
static uint16_t mgmt_resp_handle;
static uint16_t hid_input_handle;

uint16_t ble_gatt_hid_input_handle(void) { return hid_input_handle; }

// Standard boot-keyboard report descriptor (Report ID 1): an 8-byte input
// report of [modifiers, reserved, keycode x6]. Input-only — we never light
// LEDs, so no Output items, which keeps a single characteristic in play.
static const uint8_t hid_report_map[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)  -> modifier byte
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const)         -> reserved byte
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data,Array)    -> 6 keycodes
    0xC0,              // End Collection
};

// HID Information: bcdHID 1.11, no country code, remote-wake capable.
static const uint8_t hid_info[] = {0x11, 0x01, 0x00, 0x01};

// Report Reference descriptor for the input report: Report ID 1, type Input(1).
static const uint8_t hid_input_ref[] = {0x01, 0x01};

// PnP ID: vendor source 0x02 (USB-IF), VID 0x303A (Espressif), PID 0x0001,
// product version 0x0001.
static const uint8_t dis_pnp_id[] = {0x02, 0x3A, 0x30, 0x01, 0x00, 0x01, 0x00};
static const char dis_manufacturer[] = "esp-otp";

// --- Management service access ---------------------------------------------

// A persistent streaming decoder: a request frame normally arrives in one ATT
// write, but the parser resyncs on SOF so a split/garbage write self-recovers.
static protocol_parser_t mgmt_parser;

static int mgmt_cmd_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t in[PROTOCOL_MAX_FRAME];
    uint16_t in_len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, in, sizeof(in), &in_len) != 0) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    for (uint16_t i = 0; i < in_len; i++) {
        protocol_frame_t req;
        if (!protocol_parser_push(&mgmt_parser, in[i], &req)) {
            continue;
        }

        // Bound LIST packing by the negotiated MTU: ATT notify header (3) plus
        // our frame overhead (7) come off the usable payload.
        uint16_t mtu = ble_att_mtu(conn_handle);
        size_t cap = mtu > 10 ? (size_t)(mtu - 10) : 1;
        if (cap > PROTOCOL_MAX_PAYLOAD) {
            cap = PROTOCOL_MAX_PAYLOAD;
        }

        uint8_t resp[PROTOCOL_MAX_PAYLOAD];
        size_t rlen = protocol_dispatch(&req, ble_conn_authorized(conn_handle),
                                        resp, cap);

        uint8_t frame[PROTOCOL_MAX_FRAME];
        size_t flen = protocol_encode(req.type | PROTOCOL_RESP_BIT, req.seq,
                                      resp, (uint16_t)rlen, frame, sizeof(frame));
        if (!flen) {
            continue;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, flen);
        if (om && ble_gatts_notify_custom(conn_handle, mgmt_resp_handle, om) != 0) {
            ESP_LOGD(TAG, "response notify failed (client not subscribed?)");
        }
    }
    return 0;
}

// --- HID / DIS read-only access --------------------------------------------

// Serves the static read-only values (report map, HID info, PnP ID, etc.).
static int read_static(struct ble_gatt_access_ctxt *ctxt,
                       const void *data, size_t len)
{
    return os_mbuf_append(ctxt->om, data, len) == 0 ? 0
                                                    : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_report_map_access(uint16_t c, uint16_t a,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)c; (void)a; (void)arg;
    return read_static(ctxt, hid_report_map, sizeof(hid_report_map));
}

static int hid_info_access(uint16_t c, uint16_t a,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)c; (void)a; (void)arg;
    return read_static(ctxt, hid_info, sizeof(hid_info));
}

static int hid_input_access(uint16_t c, uint16_t a,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)c; (void)a; (void)arg;
    // A read before any keystroke returns an empty (all-released) report.
    static const uint8_t empty[BLE_HID_REPORT_LEN] = {0};
    return read_static(ctxt, empty, sizeof(empty));
}

static int hid_input_ref_access(uint16_t c, uint16_t a,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)c; (void)a; (void)arg;
    return read_static(ctxt, hid_input_ref, sizeof(hid_input_ref));
}

static int hid_ctrl_access(uint16_t c, uint16_t a,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)c; (void)a; (void)ctxt; (void)arg;
    // HID Control Point (suspend / exit-suspend): nothing to do, just accept.
    return 0;
}

static int dis_pnp_access(uint16_t c, uint16_t a,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)c; (void)a; (void)arg;
    return read_static(ctxt, dis_pnp_id, sizeof(dis_pnp_id));
}

static int dis_manuf_access(uint16_t c, uint16_t a,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)c; (void)a; (void)arg;
    return read_static(ctxt, dis_manufacturer, strlen(dis_manufacturer));
}

// --- Service table ----------------------------------------------------------

static const struct ble_gatt_svc_def services[] = {
    {
        // Custom management service.
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &mgmt_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &mgmt_cmd_uuid.u,
                .access_cb = mgmt_cmd_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &mgmt_resp_uuid.u,
                .access_cb = mgmt_cmd_access,  // never invoked (notify-only)
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &mgmt_resp_handle,
            },
            {0},
        },
    },
    {
        // HID-over-GATT keyboard (report protocol).
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID_HID),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(UUID_REPORT_MAP),
                .access_cb = hid_report_map_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID_REPORT),
                .access_cb = hid_input_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_READ_ENC,
                .val_handle = &hid_input_handle,
                .descriptors = (struct ble_gatt_dsc_def[]){
                    {
                        .uuid = BLE_UUID16_DECLARE(UUID_REPORT_REF),
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                        .access_cb = hid_input_ref_access,
                    },
                    {0},
                },
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID_HID_INFO),
                .access_cb = hid_info_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID_HID_CTRL),
                .access_cb = hid_ctrl_access,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {0},
        },
    },
    {
        // Device Information.
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID_DIS),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(UUID_PNP_ID),
                .access_cb = dis_pnp_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID_MANUFACTURER),
                .access_cb = dis_manuf_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {0},
        },
    },
    {0},
};

int ble_gatt_init(void)
{
    protocol_parser_reset(&mgmt_parser);

    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(services);
    if (rc != 0) {
        return rc;
    }
    rc = ble_gatts_add_svcs(services);
    if (rc != 0) {
        return rc;
    }
    ESP_LOGI(TAG, "GATT services registered");
    return 0;
}
