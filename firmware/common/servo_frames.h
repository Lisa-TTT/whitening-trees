#ifndef SERVO_BENCH_PROTOCOL_H
#define SERVO_BENCH_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RC_FRAME_SIZE 12u

enum { RC_STOP = 2, SERVO_PING = 0x30, SERVO_JOG = 0x31,
       SERVO_STATUS = 0xb0, SERVO_BENCH_TAG = 0x5342 };

typedef struct { uint8_t type; uint16_t seq; int16_t left, right; } rc_frame_t;
typedef struct { uint8_t bytes[RC_FRAME_SIZE]; uint8_t used; } rc_parser_t;

void rc_encode(uint8_t out[RC_FRAME_SIZE], const rc_frame_t *frame);
int rc_feed(rc_parser_t *parser, uint8_t byte, rc_frame_t *frame);

#ifdef __cplusplus
}
#endif
#endif\n