// BLE connectivity: NimBLE peripheral exposing two independent functions.
//
//  1. A custom *management* GATT service that carries the same framed wire
//     protocol as USB (see PROTOCOL.md): the app/web page writes a request
//     frame to the Command characteristic and gets the response as a
//     notification on the Response characteristic. Mutating commands require a
//     bonded/encrypted link; SET_TIME is accepted unauthenticated so a phone
//     can seed the clock right after connecting.
//
//  2. A standard HID-over-GATT *keyboard* so the device can "type" the current
//     TOTP code into a phone/laptop on a long-press (blecon_type_code()).
//
// Pairing is numeric-comparison and MITM-required: under LE Secure Connections
// the phone and the OLED show the same 6-digit code and the user confirms the
// match with the button (no Just Works fallback; a legacy host that can't do
// LESC falls back to typing a displayed passkey). New bonds are only accepted
// while a button-gated pairing window is open, so a configured device can't be
// hijacked. Bonds persist in NVS so a paired host reconnects without
// re-pairing.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the BT controller + NimBLE host, register the management and HID
// services, and start advertising. Bonds are restored from NVS. Returns once
// the host task is running (advertising starts asynchronously).
esp_err_t blecon_init(void);

// Queue an ASCII string to be typed over the HID keyboard. Non-blocking: the
// keystrokes are emitted by a background task so the UI stays responsive.
// Returns ESP_ERR_INVALID_STATE if no host is connected/subscribed to the HID
// input report, ESP_ERR_NO_MEM if the type queue is full.
esp_err_t blecon_type_code(const char *str);

// Number of hosts currently subscribed as HID keyboards (candidates for the
// keystroke target).
size_t blecon_hid_host_count(void);

// Advance the active HID keystroke target to the next subscribed host
// (round-robin). No-op with fewer than two candidates.
void blecon_cycle_hid_target(void);

// Write a short label for the active HID target into `buf` ("none" if no host
// is subscribed) and return the number of subscribed hosts. Used by the UI to
// show/cycle the typing destination.
size_t blecon_hid_target_label(char *buf, size_t cap);

// If a pairing passkey is currently being shown for the user to type on the
// connecting host, write it (0..999999) to *passkey and return true; otherwise
// return false. Polled by the UI to render the pairing screen.
bool blecon_pairing_passkey(uint32_t *passkey);

// If a numeric-comparison pairing is awaiting the user's confirmation, write the
// 6-digit code (0..999999) to *code and return true; otherwise return false.
// The same code is shown on the connecting phone — the user checks they match
// and calls blecon_confirm_pairing(). Polled by the UI to render the confirm
// prompt.
bool blecon_pairing_confirm_pending(uint32_t *code);

// Answer a pending numeric-comparison prompt: accept=true completes the pairing
// (codes matched), accept=false rejects it. Call from the button handler while
// blecon_pairing_confirm_pending() is true; a no-op otherwise.
void blecon_confirm_pairing(bool accept);

// Open the pairing window: until it times out (or a host finishes pairing), a
// new peer may bond. Call on the long-press gesture. Outside the window new
// pairings are refused, so a configured device can't be hijacked.
void blecon_open_pairing(void);

// True while the pairing window is open. Polled by the UI to show a "pairing"
// hint before a passkey appears.
bool blecon_pairing_open(void);

#ifdef __cplusplus
}
#endif
