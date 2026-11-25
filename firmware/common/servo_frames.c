#include "servo_frames.h"

static uint16_t crc16(const uint8_t *bytes, size_t length)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)bytes[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint16_t next = (uint16_t)(crc << 1);
            crc = (crc & 0x8000) ? (uint16_t)(next ^ 0x1021) : next;
        }
    }
    return crc;
}

static void put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static uint16_t get_u16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int16_t get_s16(const uint8_t *bytes)
{
    uint16_t value = get_u16(bytes);
    return (int16_t)(value < 0x8000u ? (int32_t)value : (int32_t)value - 65536);
}

void rc_encode(uint8_t out[RC_FRAME_SIZE], const rc_frame_t *frame)
{
    out[0] = 0xaa;
    out[1] = 0x55;
    out[2] = 1;
    out[3] = frame->type;
    put_u16(out + 4, frame->seq);
    put_u16(out + 6, (uint16_t)frame->left);
    put_u16(out + 8, (uint16_t)frame->right);
    put_u16(out + 10, crc16(out, 10));
}

int rc_feed(rc_parser_t *parser, uint8_t byte, rc_frame_t *frame)
{
    if (!parser->used && byte != 0xaa) return 0;
    parser->bytes[parser->used++] = byte;
    if (parser->used != RC_FRAME_SIZE) return 0;
    const uint8_t *data = parser->bytes;
    if (data[1] == 0x55 && data[2] == 1 && get_u16(data + 10) == crc16(data, 10)) {
        frame->type = data[3];
        frame->seq = get_u16(data + 4);
        frame->left = get_s16(data + 6);
        frame->right = get_s16(data + 8);
        parser->used = 0;
        return 1;
    }
    unsigned offset = 1;
    while (offset < RC_FRAME_SIZE && data[offset] != 0xaa) ++offset;
    parser->used = (uint8_t)(RC_FRAME_SIZE - offset);
    for (unsigned i = 0; i < parser->used; ++i) parser->bytes[i] = data[offset + i];
    return 0;
}\n