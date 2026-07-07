#pragma once

#include <stdint.h>

// WLtoys 6401 yellow-wire packet, per PROTOCOL_ANALYSIS.md §6 (all [CAP]):
//
//   byte 0 : 0x66   start marker      (constant)
//   byte 1 : STEER  steering          (0x80 centre, 0x59 left, 0xA6 right)
//   byte 2 : THR    throttle          (0x80 neutral, 0xFF forward, 0x00 reverse)
//   byte 3 : 0x80   constant
//   byte 4 : 0x00   constant
//   byte 5 : 0x00   constant
//   byte 6 : CHK    XOR checksum = byte1 ^ byte2 ^ byte3
//   byte 7 : 0x99   end marker        (constant)
//
// This file is free of ESP-IDF headers so protocol_build() can be compiled
// and unit-tested on a host PC and diffed against the captured bytes.

#define PROTOCOL_PACKET_LEN 8

#define PKT_START  0x66
#define PKT_END    0x99
#define PKT_CONST3 0x80
#define PKT_CONST4 0x00
#define PKT_CONST5 0x00

#define STEER_CENTER 0x80
#define STEER_LEFT   0x59 // verified partial deflection [CAP]
#define STEER_RIGHT  0xA6 // verified partial deflection [CAP]

#define THR_NEUTRAL 0x80
#define THR_FORWARD 0xFF // verified full forward [CAP]
#define THR_REVERSE 0x00 // verified full reverse [CAP]

// Builds the 8-byte yellow-wire packet for the given steer/throttle fields
// into out[0..7], computing the checksum dynamically.
void protocol_build(uint8_t steer, uint8_t thr, uint8_t out[PROTOCOL_PACKET_LEN]);
