#include <string.h>

#include "protocol.h"
#include "unity.h"

TEST_CASE("crc16 ccitt-false vector", "[protocol]")
{
    TEST_ASSERT_EQUAL_HEX16(0x29B1, protocol_crc16((const uint8_t *)"123456789", 9));
}

TEST_CASE("encode/decode round-trip", "[protocol]")
{
    uint8_t payload[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    uint8_t frame[PROTOCOL_MAX_FRAME];
    size_t n = protocol_encode(OP_ADD, 0x11, payload, sizeof(payload), frame, sizeof(frame));
    TEST_ASSERT_EQUAL_UINT(7 + 5, n);

    protocol_parser_t p;
    protocol_parser_reset(&p);
    protocol_frame_t out;
    bool done = false;
    for (size_t i = 0; i < n; i++) {
        done = protocol_parser_push(&p, frame[i], &out) || done;
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT8(OP_ADD, out.type);
    TEST_ASSERT_EQUAL_UINT8(0x11, out.seq);
    TEST_ASSERT_EQUAL_UINT(5, out.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, out.payload, 5);
}

TEST_CASE("corrupted frame is rejected", "[protocol]")
{
    uint8_t frame[PROTOCOL_MAX_FRAME];
    size_t n = protocol_encode(OP_PING, 1, NULL, 0, frame, sizeof(frame));
    frame[5] ^= 0xFF;  // corrupt a CRC byte

    protocol_parser_t p;
    protocol_parser_reset(&p);
    protocol_frame_t out;
    bool done = false;
    for (size_t i = 0; i < n; i++) {
        done = protocol_parser_push(&p, frame[i], &out) || done;
    }
    TEST_ASSERT_FALSE(done);
}

TEST_CASE("parser resyncs after garbage", "[protocol]")
{
    uint8_t frame[PROTOCOL_MAX_FRAME];
    size_t n = protocol_encode(OP_GET_INFO, 7, NULL, 0, frame, sizeof(frame));

    protocol_parser_t p;
    protocol_parser_reset(&p);
    protocol_frame_t out;
    bool done = false;
    const uint8_t junk[] = {0x00, 0xAA, 0xFF, 0x12};
    for (size_t i = 0; i < sizeof(junk); i++) {
        protocol_parser_push(&p, junk[i], &out);
    }
    for (size_t i = 0; i < n; i++) {
        done = protocol_parser_push(&p, frame[i], &out) || done;
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT8(OP_GET_INFO, out.type);
    TEST_ASSERT_EQUAL_UINT8(7, out.seq);
}
