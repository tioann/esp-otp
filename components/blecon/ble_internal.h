// Private glue shared between blecon.c (host/GAP/typing) and ble_gatt.c
// (service definitions + access callbacks). Not part of the public API.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host/ble_hs.h"

// HID input report payload: [modifiers, reserved, keycode x6]. Same layout for
// report-protocol and boot-protocol keyboard reports.
#define BLE_HID_REPORT_LEN 8

// The custom management service's 128-bit UUID (defined in ble_gatt.c). blecon.c
// puts it in the advertising payload so a scanner can recognise an esp-otp
// device by service type even after its GAP name is changed.
extern const ble_uuid128_t mgmt_svc_uuid;

// Register the management + HID + device-info GATT services. Call after
// ble_svc_gap/gatt init and before advertising. Captures the value handles
// used for notifications (see below).
int ble_gatt_init(void);

// Value handle of the HID input report characteristic for the host's currently
// negotiated protocol mode (report vs boot). 0 until the GATT layer is
// registered. Used by the typing task to notify keystrokes.
uint16_t ble_gatt_hid_input_handle(void);

// True if `conn_handle` has an encrypted link (used to authorize mutating
// management commands per PROTOCOL.md).
bool ble_conn_authorized(uint16_t conn_handle);

#ifdef __cplusplus
extern "C" {
#endif
#ifdef __cplusplus
}
#endif
