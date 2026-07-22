#include "protocol.h"

#include <string.h>

uint16_t protocol_crc16(const uint8_t *data, size_t n)
{
    // CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection.
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t protocol_encode(uint8_t type, uint8_t seq, const uint8_t *payload,
                       uint16_t plen, uint8_t *out, size_t out_cap)
{
    if (plen > PROTOCOL_MAX_PAYLOAD) {
        return 0;
    }
    uint16_t body_len = 2 + plen;             // TYPE + SEQ + payload
    size_t frame_len = 1 + 2 + body_len + 2;  // SOF + LEN + body + CRC
    if (out == NULL || out_cap < frame_len) {
        return 0;
    }

    out[0] = PROTOCOL_SOF;
    out[1] = (uint8_t)(body_len & 0xFF);
    out[2] = (uint8_t)(body_len >> 8);
    out[3] = type;
    out[4] = seq;
    if (plen && payload) {
        memcpy(&out[5], payload, plen);
    }
    uint16_t crc = protocol_crc16(&out[3], body_len);  // over TYPE+SEQ+payload
    out[5 + plen] = (uint8_t)(crc & 0xFF);
    out[6 + plen] = (uint8_t)(crc >> 8);
    return frame_len;
}

enum { S_SOF = 0, S_LEN0, S_LEN1, S_BODY };

void protocol_parser_reset(protocol_parser_t *p)
{
    memset(p, 0, sizeof(*p));
    p->state = S_SOF;
}

bool protocol_parser_push(protocol_parser_t *p, uint8_t byte, protocol_frame_t *out)
{
    switch (p->state) {
        case S_SOF:
            if (byte == PROTOCOL_SOF) {
                p->state = S_LEN0;
            }
            return false;

        case S_LEN0:
            p->body_len = byte;
            p->state = S_LEN1;
            return false;

        case S_LEN1:
            p->body_len |= (uint16_t)byte << 8;
            // body_len must cover at least TYPE+SEQ and fit our buffer.
            if (p->body_len < 2 || p->body_len > 2 + PROTOCOL_MAX_PAYLOAD) {
                protocol_parser_reset(p);
                return false;
            }
            p->got = 0;
            p->state = S_BODY;
            return false;

        case S_BODY:
            // Collect body (body_len bytes) followed by the 2 CRC bytes.
            p->buf[p->got++] = byte;
            if (p->got < p->body_len + 2) {
                return false;
            }
            // Full frame in buf: [TYPE SEQ payload...][crc_lo crc_hi]
            {
                uint16_t crc_calc = protocol_crc16(p->buf, p->body_len);
                uint16_t crc_recv = (uint16_t)p->buf[p->body_len] |
                                    ((uint16_t)p->buf[p->body_len + 1] << 8);
                bool ok = false;
                if (crc_calc == crc_recv) {
                    out->type = p->buf[0];
                    out->seq = p->buf[1];
                    out->len = p->body_len - 2;
                    if (out->len) {
                        memcpy(out->payload, &p->buf[2], out->len);
                    }
                    ok = true;
                }
                protocol_parser_reset(p);
                return ok;
            }

        default:
            protocol_parser_reset(p);
            return false;
    }
}
