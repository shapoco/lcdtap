#pragma once

#include <cstdint>

// TGA/PackBits style RLE for RGB565 pixel streams.
//
// Packet layout (repeated):
//   control byte c:
//     c & 0x80 set   -> run:     next 2 bytes (RGB565 LE) repeat (c & 0x7F)+1
//                       times (1..128 pixels)
//     c & 0x80 clear -> literal: next ((c & 0x7F)+1)*2 bytes are raw pixels
//                       (1..128 pixels, LE)

// Maximum encoded size for one segment of up to 128 pixels: one literal
// control byte plus 128 raw pixels.
inline constexpr int RLE_SEG_MAX_PIXELS = 128;
inline constexpr int RLE_SEG_MAX_BYTES = 1 + 2 * RLE_SEG_MAX_PIXELS;

// Encode n (1..RLE_SEG_MAX_PIXELS) RGB565 pixels into RLE packets.
// Returns bytes written (<= 1 + 2 * n).
int rleEncodeSegment(const uint16_t* px, int n, uint8_t* out);
