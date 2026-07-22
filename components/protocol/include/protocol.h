// esp-otp wire protocol: framing + opcode/status definitions.
//
// The framing core (CRC, encode, streaming decode) is dependency-free and
// host-testable. Command dispatch (which touches the store/clock) lives in
// dispatch.c and is built only for the target. See PROTOCOL.md for the
// on-the-wire contract.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Firmware/protocol version, reported by GET_INFO.
#define OTP_FW_MAJOR 0
#define OTP_FW_MINOR 1
#define OTP_FW_PATCH 0

#define PROTOCOL_SOF          0x7E
#define PROTOCOL_MAX_PAYLOAD  250
#define PROTOCOL_RESP_BIT     0x80
// SOF(1) + LEN(2) + TYPE(1) + SEQ(1) + payload + CRC(2)
#define PROTOCOL_MAX_FRAME    (7 + PROTOCOL_MAX_PAYLOAD)

// Request opcodes.
typedef enum {
    OP_PING     = 0x01,
    OP_GET_INFO = 0x02,
    OP_SET_TIME = 0x03,
    OP_LIST     = 0x04,
    OP_ADD      = 0x05,
    OP_REMOVE   = 0x06,
    OP_RENAME   = 0x07,
    OP_GET_SCREEN = 0x08,
    OP_INJECT_BUTTON = 0x09,
    OP_REBOOT   = 0x0A,
    OP_MOVE     = 0x0B,
    OP_SET_NAME = 0x0C,
} protocol_op_t;

// Longest BLE device name accepted by OP_SET_NAME. Kept small so the name fits
// the advertising scan-response payload AND rides in every GET_INFO response
// without depending on the negotiated MTU.
#define PROTOCOL_DEVICE_NAME_MAX 20

// Button gestures for OP_INJECT_BUTTON payload[0]; mirror button_event_t so a
// host can drive the UI as if the physical button were pressed.
typedef enum {
    OTP_BTN_SINGLE = 1,
    OTP_BTN_DOUBLE = 2,
    OTP_BTN_LONG   = 3,
} protocol_button_t;

// Response status byte.
typedef enum {
    ST_OK             = 0x00,
    ST_BAD_REQUEST    = 0x01,
    ST_FULL           = 0x02,
    ST_NOT_FOUND      = 0x03,
    ST_INVALID_ARG    = 0x04,
    ST_NOT_AUTHORIZED = 0x05,
    ST_CRC_ERROR      = 0x06,
    ST_UNSUPPORTED    = 0x07,
} protocol_status_t;

// A decoded frame (request or response).
typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t payload[PROTOCOL_MAX_PAYLOAD];
    uint16_t len;   // payload length
} protocol_frame_t;

// CRC-16/CCITT-FALSE over `data`.
uint16_t protocol_crc16(const uint8_t *data, size_t n);

// Encode a frame into `out` (capacity `out_cap`). Returns the encoded length,
// or 0 on overflow / bad args.
size_t protocol_encode(uint8_t type, uint8_t seq, const uint8_t *payload,
                       uint16_t plen, uint8_t *out, size_t out_cap);

// Streaming decoder: feed bytes one at a time. Resynchronises on SOF and
// validates CRC. Returns true and fills `out` when a complete, valid frame
// has been received; returns false otherwise (including on CRC failure, which
// is silently dropped — the caller can time out).
typedef struct {
    uint8_t state;
    uint16_t body_len;   // TYPE+SEQ+PAYLOAD length from LEN field
    uint16_t got;        // bytes collected into buf
    uint8_t buf[2 + PROTOCOL_MAX_PAYLOAD + 2];  // body + CRC
} protocol_parser_t;

void protocol_parser_reset(protocol_parser_t *p);
bool protocol_parser_push(protocol_parser_t *p, uint8_t byte, protocol_frame_t *out);

// Handle a decoded request and build the response payload (status byte first)
// into `resp`, returning its length. `authorized` gates mutating commands on
// unbonded BLE links; `resp_cap` bounds how many LIST entries are packed (use
// the negotiated MTU on BLE). Defined in dispatch.c (target-only).
size_t protocol_dispatch(const protocol_frame_t *req, bool authorized,
                         uint8_t *resp, size_t resp_cap);

// OP_SET_NAME handler, registered by the BLE layer at startup (protocol can't
// depend on blecon without a dependency cycle, so the dependency is inverted via
// this hook). `name` is NUL-terminated, 1..PROTOCOL_DEVICE_NAME_MAX bytes; the
// implementation persists + applies it and returns true on success. NULL until
// registered — OP_SET_NAME then answers UNSUPPORTED.
typedef bool (*protocol_set_name_fn)(const char *name);
void protocol_register_set_name(protocol_set_name_fn fn);

// Companion getter for the current device name, used by GET_INFO. Returns a
// NUL-terminated, stable pointer to the live name. NULL until registered — the
// GET_INFO name field is then empty (length 0).
typedef const char *(*protocol_get_name_fn)(void);
void protocol_register_get_name(protocol_get_name_fn fn);

#ifdef __cplusplus
}
#endif
