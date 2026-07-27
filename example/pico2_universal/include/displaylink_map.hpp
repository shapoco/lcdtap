#pragma once

// Pure output-mapping helpers for the DisplayLink partial-update pump,
// mirroring LcdTap::fillScanline's fixed-point math (see OutputMapInfo in
// lcdtap.hpp). Kept free of MCU dependencies so the host-side test
// (test_host/displaylink_span_test.cpp) exercises exactly this code.

#include <cstdint>

#include "lcdtap/lcdtap.hpp"

// First output line whose mapped (crop-relative) source line index is t.
// The mapping is srcLine(y) = ((y - destY) * stepV) >> 16, monotonic, so the
// duplicate group of t is [firstLine(t), firstLine(t+1) - 1].
static inline uint32_t groupFirstLine(const lcdtap::OutputMapInfo& mi,
                                      uint32_t t) {
  if (mi.stepV == 0) return mi.destY;
  return mi.destY +
         static_cast<uint32_t>(
             ((static_cast<uint64_t>(t) << 16) + mi.stepV - 1u) / mi.stepV);
}

// Map a crop-relative source index range [sc0, sc1] along the output line
// direction to an output x span. mirrored selects the reversed orientations
// (rot=2 columns, rot=1 rows); fillScanline guarantees the reversed sampling
// is the pixel-exact mirror of the forward one, so reflection is exact. The
// ±1 expansion remains as cheap insurance against future drift.
static inline bool srcRangeToSpan(const lcdtap::OutputMapInfo& mi, uint32_t sc0,
                                  uint32_t sc1, bool mirrored, uint32_t* x0,
                                  uint32_t* x1) {
  if (mi.stepH == 0 || mi.destW == 0) return false;
  uint32_t a = static_cast<uint32_t>(
      ((static_cast<uint64_t>(sc0) << 16) + mi.stepH - 1u) / mi.stepH);
  uint32_t b = static_cast<uint32_t>(
      ((static_cast<uint64_t>(sc1 + 1u) << 16) + mi.stepH - 1u) / mi.stepH);
  int32_t r0 = static_cast<int32_t>(a) - 1;
  int32_t r1 = static_cast<int32_t>(b);  // (b - 1) + 1 rounding guard
  if (r0 < 0) r0 = 0;
  if (r1 > static_cast<int32_t>(mi.destW) - 1) r1 = mi.destW - 1;
  if (r0 > r1) return false;
  if (mirrored) {
    int32_t m0 = static_cast<int32_t>(mi.destW) - 1 - r1;
    int32_t m1 = static_cast<int32_t>(mi.destW) - 1 - r0;
    r0 = m0;
    r1 = m1;
  }
  *x0 = mi.destX + static_cast<uint32_t>(r0);
  *x1 = mi.destX + static_cast<uint32_t>(r1);
  return true;
}
