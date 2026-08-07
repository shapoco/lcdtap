#pragma once

// Test pattern generator (platform independent, host-testable).
//
// Pixel values come from a counter-based hash PRNG keyed by the physical
// framebuffer pixel index, so any pixel's expected value can be recomputed
// in O(1) during streaming verification — no expected-frame buffer and no
// dependency on generation order.

#include <cstddef>
#include <cstdint>

#include "lcdtap/config.hpp"

namespace testrig {

// SplitMix32-style finalizer. Statistically good enough for test patterns
// and cheap enough to run per pixel while draining the readback stream.
inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

struct Rgb888 {
  uint8_t r, g, b;
};

struct PatternParams {
  lcdtap::InterfaceFormat format;
  uint16_t physW;  // physical framebuffer width (pattern index stride)
  uint32_t seed;
};

// Canonical 8-bit channels for a physical pixel. Wire encoders emit the top
// bits of these; the expected framebuffer value is derived from the same
// channels, so generator and verifier can never disagree.
inline Rgb888 canonicalColor(uint32_t seed, uint32_t physIndex) {
  // Golden-ratio index mixing decorrelates neighbouring pixels even for
  // adjacent seeds.
  uint32_t h = hash32(seed ^ (physIndex * 0x9E3779B9u));
  return Rgb888{static_cast<uint8_t>(h >> 16), static_cast<uint8_t>(h >> 8),
                static_cast<uint8_t>(h)};
}

// GRAY1 pixels are on/off only; use the red channel MSB as the bit.
inline bool canonicalGray1(uint32_t seed, uint32_t physIndex) {
  return (canonicalColor(seed, physIndex).r & 0x80u) != 0;
}

// Wire bytes for w pixels of one row. For GRAY1 this is the bytes of one
// 8-row page (w columns). RGB444 and RGB111 require w to be even.
size_t wireBytesPerRow(lcdtap::InterfaceFormat fmt, uint16_t w);

// Total wire bytes for a w x rows rectangle (GRAY1: rows must be a multiple
// of 8; bytes cover rows/8 pages).
size_t wireBytesForRect(lcdtap::InterfaceFormat fmt, uint16_t w, uint16_t rows);

// Build the RAMWR wire bytes for pixel rows [y0, y0+rows) of a window whose
// left edge is x0 and width is w. Pattern indices are physical framebuffer
// coordinates (y * physW + x), so a windowed send (e.g. AUTO trim) still
// produces the same expected values as a full-frame send would at the same
// location. For GRAY1, y0 and rows must be multiples of 8.
// Returns the number of bytes written.
size_t buildWireRect(const PatternParams& p, uint16_t x0, uint16_t y0,
                     uint16_t w, uint16_t rows, uint8_t* out);

// One row (one page for GRAY1) of a solid mid-gray dummy pattern.
size_t buildSolidRow(lcdtap::InterfaceFormat fmt, uint16_t w, uint8_t* out);

// Expected framebuffer RGB565 value for a physical pixel. rbSwap mirrors the
// target's cachedBGR (MADCTL/SETREMAP BGR bit XOR config.swapRB); the rig
// always sends BGR=0, so pass the target's swapRB setting.
// Note: display inversion needs no handling here — the target XORs pixels on
// write and getframebuffer XORs them again on readout with the same flag, so
// the readback always matches the non-inverted decode.
uint16_t expectedRgb565(const PatternParams& p, uint32_t physIndex,
                        bool rbSwap);

// Comparison mask: RGB565 bits that are fully determined by the wire format
// (top N bits per channel). Masked compare removes any dependence on the
// target's low-bit expansion policy, per the test spec.
uint16_t compareMask(lcdtap::InterfaceFormat fmt, bool rbSwap);

// Deterministic text pattern for character LCD vectors. Fills out[0..cols-1]
// with printable ASCII (0x21..0x7E). No terminator is written.
void buildTextRow(uint32_t seed, uint16_t row, uint16_t cols, char* out);

}  // namespace testrig
