#include <lcdtap/hot.hpp>
#include <lcdtap/lcdtap.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#ifdef PICO_RP2350
#include "hardware/interp.h"
#endif

#include "controller_base.hpp"
#include "ili9341_controller.hpp"
#include "ssd1306_controller.hpp"
#include "ssd1331_controller.hpp"
#include "st7789_controller.hpp"

namespace lcdtap {

//=============================================================================
// ControllerBase common implementation
//=============================================================================

void ControllerBase::calcScaleParams() {
  uint16_t videoW = config.dviWidth;
  uint16_t videoH = config.dviHeight;

  uint16_t buffW = config.buffWidth;
  uint16_t buffH = config.buffHeight;

  int16_t trimW = buffW;
  int16_t trimH = buffH;
  if (config.trimMode != TrimMode::OFF) {
    trimW = config.trimWidth;
    trimH = config.trimHeight;
  }

  int16_t trimX = 0;
  int16_t trimY = 0;
  if (config.trimMode != TrimMode::OFF) {
    trimX = config.trimX;
    trimY = config.trimY;
  }

  // Trim area must be within the buffer area
  int16_t srcW = trimW;
  int16_t srcH = trimH;
  if (trimX < 0) {
    srcW += trimX;
    trimX = 0;
  } else if (trimX + srcW > buffW) {
    srcW = buffW - trimX;
  }
  if (trimY < 0) {
    srcH += trimY;
    trimY = 0;
  } else if (trimY + srcH > buffH) {
    srcH = buffH - trimY;
  }

  if (srcW <= 0 || srcH <= 0) {
    // No valid trim area; disable scaling and output
    outDestX = 0;
    outDestY = 0;
    outDestW = 0;
    outDestH = 0;
    outSrcX = 0;
    outSrcY = 0;
    outSrcW = 0;
    outSrcH = 0;
    outSrcStepH = 0;
    outSrcStepV = 0;
    bumpEpoch();
    return;
  }

  // rot=1,3: LCD width and height appear swapped
  if ((config.outputRotation & 1) != 0) {
    std::swap(buffW, buffH);
    std::swap(srcW, srcH);
  }

  switch (config.scaleMode) {
    case ScaleMode::STRETCH:
      // Fill the entire video area (ignore aspect ratio)
      outDestX = 0;
      outDestY = 0;
      outDestW = videoW;
      outDestH = videoH;
      break;

    case ScaleMode::FIT:
      // Scale to fit the video area while preserving aspect ratio; add black
      // padding if needed
      if ((uint32_t)srcW * videoH > (uint32_t)videoW * srcH) {
        outDestW = videoW;
        outDestH = (uint16_t)((uint32_t)videoW * srcH / srcW);
      } else {
        outDestH = videoH;
        outDestW = (uint16_t)((uint32_t)videoH * srcW / srcH);
      }
      outDestX = (videoW - outDestW) / 2;
      outDestY = (videoH - outDestH) / 2;
      break;

    case ScaleMode::INTEGRAL:
    case ScaleMode::OFF: {
      // Scale by the largest integer factor that fits in the video area; add
      // black padding if needed
      uint16_t scale = 1;
      if (config.scaleMode == ScaleMode::INTEGRAL) {
        uint16_t scaleH = videoW / srcW;
        uint16_t scaleV = videoH / srcH;
        scale = scaleH < scaleV ? scaleH : scaleV;
        if (scale == 0) scale = 1;
      }
      outDestW = srcW * scale;
      outDestH = srcH * scale;
      outDestX = (videoW - outDestW) / 2;
      outDestY = (videoH - outDestH) / 2;
      break;
    }
  }

  outSrcX = static_cast<uint16_t>(trimX);
  outSrcY = static_cast<uint16_t>(trimY);
  outSrcW = static_cast<uint16_t>(trimW);
  outSrcH = static_cast<uint16_t>(trimH);
  outSrcStepH = ((uint32_t)srcW << FIXPT_PREC) / outDestW;
  outSrcStepV = ((uint32_t)srcH << FIXPT_PREC) / outDestH;
  bumpEpoch();
}

void ControllerBase::log(const char* msg) const {
  if (host.log) host.log(host.userData, msg);
}

void ControllerBase::resetCommon() {
  bumpEpoch();
  sleeping = true;
  displayOn = false;
  setInverted(false);
  cachedLittleEndian = false;
  interfaceFormat = getDefaultInterfaceFormat(config.controllerFamily);
  currentCmd = 0x00;
  cmdDataLen = 0;
  casetXS = 0;
  casetXE = config.buffWidth - 1;
  rasetYS = 0;
  rasetYE = config.buffHeight - 1;
  ramwrX = 0;
  ramwrY = 0;
  ramwrBufLen = 0;
  if (config.trimMode == TrimMode::AUTO) {
    config.trimX = 0;
    config.trimY = 0;
    config.trimWidth = 0;
    config.trimHeight = 0;
  }
  updateWriteCache();
}

// Set the output inversion state (true = inverted, false = normal)
void ControllerBase::setInverted(bool inv) {
  inverted = inv;
  cachedInverter = inverted ? 0xFFFFu : 0x0000u;
  bumpEpoch();
}

// Expand the trim area to include the specified rectangle (logical coordinates)
void ControllerBase::expandTrimX(uint16_t x0, uint16_t x1) {
  if (config.trimMode != TrimMode::AUTO) return;
  if (x0 >= x1) return;                // empty rectangle
  if (x1 >= config.buffWidth) return;  // out of bounds
  if (config.trimWidth == 0) {
    // No trim area yet; set to the new rectangle
    config.trimX = x0;
    config.trimWidth = static_cast<uint16_t>(x1 - x0 + 1);
  } else {
    config.trimX = std::min(config.trimX, x0);
    config.trimWidth =
        std::max(config.trimWidth, (uint16_t)(x1 - config.trimX + 1));
  }
  calcScaleParams();
}

// Expand the trim area to include the specified rectangle (logical coordinates)
void ControllerBase::expandTrimY(uint16_t y0, uint16_t y1) {
  if (config.trimMode != TrimMode::AUTO) return;
  if (y0 >= y1) return;                 // empty rectangle
  if (y1 >= config.buffHeight) return;  // out of bounds
  if (config.trimHeight == 0) {
    // No trim area yet; set to the new rectangle
    config.trimY = y0;
    config.trimHeight = static_cast<uint16_t>(y1 - y0 + 1);
  } else {
    config.trimY = std::min(config.trimY, y0);
    config.trimHeight =
        std::max(config.trimHeight, (uint16_t)(y1 - config.trimY + 1));
  }
  calcScaleParams();
}

// Process all RAMWR data at once (moves switch(interfaceFormat) outside the
// loop). Dispatches to the template body so the tracking-off path keeps its
// historical codegen (no per-pixel cost when dirty tracking is disabled).
void ControllerBase::processRamwrData(const uint8_t* data, uint32_t numBytes,
                                      uint32_t stride) {
  if (dirtyTracking) {
    processRamwrDataImpl<true>(data, numBytes, stride);
  } else {
    processRamwrDataImpl<false>(data, numBytes, stride);
  }
}

template <bool kDirty, uint32_t kStride>
void LCDTAP_INLINE ControllerBase::ramwrRgb565RunImpl(const uint8_t* data,
                                                      uint32_t numPixels,
                                                      uint32_t strideArg,
                                                      DirtyAcc& acc) {
  const uint32_t stride = (kStride != 0) ? kStride : strideArg;
  const uint16_t inv = cachedInverter;
  const uint32_t inv32 = (static_cast<uint32_t>(inv) << 16) | inv;
  const int32_t hStep = cachedHStep;
  const bool bgr = cachedBGR;
  const bool le = cachedLittleEndian;
  const uint8_t* p = data;
  uint32_t x = ramwrX;
  uint16_t* ptr = writePtr;

  auto fetchPx = [&]() [[gnu::always_inline]] {
    uint_fast16_t b0 = p[0];
    uint_fast16_t b1 = p[stride];
    p += stride * 2u;
    uint_fast16_t px = le ? (b0 | (b1 << 8)) : ((b0 << 8) | b1);
    if (bgr) {
      px = ((px << 11) & 0xF800u) | (px & 0x07E0u) | ((px >> 11) & 0x1Fu);
    }
    return static_cast<uint_fast16_t>(px ^ inv);
  };

  while (numPixels != 0) {
    // casetXE < x only happens with a degenerate CASET window (XE < XS);
    // fall back to single-pixel runs there, matching the per-pixel path.
    int32_t rowRemain =
        static_cast<int32_t>(casetXE) - static_cast<int32_t>(x) + 1;
    uint32_t run = rowRemain <= 0 ? 1u : static_cast<uint32_t>(rowRemain);
    if (run > numPixels) run = numPixels;
    if constexpr (kDirty) {
      // Cap runs at 64px segment boundaries so the diff accumulation keeps
      // the per-pixel path's segment granularity.
      uint32_t segRemain = 64u - (x & 63u);
      if (run > segRemain) run = segRemain;
    }
    numPixels -= run;
    x += run;

    uint32_t n = run;
    if (hStep == 1) {
      if ((reinterpret_cast<uintptr_t>(ptr) & 3u) != 0) {
        uint_fast16_t px = fetchPx();
        if constexpr (kDirty) {
          acc.diff |= static_cast<uint32_t>(*ptr ^ px);
        }
        *ptr++ = static_cast<uint16_t>(px);
        --n;
      }
      uint32_t* ptr32 = reinterpret_cast<uint32_t*>(ptr);
      for (uint32_t k = n / 2u; k != 0; --k) {
        uint_fast32_t b0 = p[0];
        uint_fast32_t b1 = p[stride];
        uint_fast32_t b2 = p[stride * 2u];
        uint_fast32_t b3 = p[stride * 3u];
        p += stride * 4u;
        uint32_t w = le ? (b0 | (b1 << 8) | (b2 << 16) | (b3 << 24))
                        : ((b0 << 8) | b1 | (b2 << 24) | (b3 << 16));
        if (bgr) {
          w = ((w << 11) & 0xF800F800u) | (w & 0x07E007E0u) |
              ((w >> 11) & 0x001F001Fu);
        }
        w ^= inv32;
        if constexpr (kDirty) {
          acc.diff |= *ptr32 ^ w;
        }
        *ptr32++ = w;
      }
      ptr = reinterpret_cast<uint16_t*>(ptr32);
      if (n & 1u) {
        uint_fast16_t px = fetchPx();
        if constexpr (kDirty) {
          acc.diff |= static_cast<uint32_t>(*ptr ^ px);
        }
        *ptr++ = static_cast<uint16_t>(px);
      }
    } else {
      for (uint32_t k = n; k != 0; --k) {
        uint_fast16_t px = fetchPx();
        if constexpr (kDirty) {
          acc.diff |= static_cast<uint32_t>(*ptr ^ px);
        }
        *ptr = static_cast<uint16_t>(px);
        ptr += hStep;
      }
    }

    if constexpr (kDirty) {
      if (acc.diff != 0) {
        acc.segBits |= 1u << ((x - 1u) >> 6);
        acc.diff = 0;
      }
    }
    if (x > casetXE) {
      if constexpr (kDirty) {
        if (acc.segBits != 0) {
          markLogicalRowDirty(ramwrY, acc.segBits);
          acc.segBits = 0;
        }
      }
      x = casetXS;
      if (++ramwrY > rasetYE) ramwrY = rasetYS;
      ptr = frameBuffer + physIndex(x, ramwrY);
    }
  }
  ramwrX = static_cast<uint16_t>(x);
  writePtr = ptr;
}

template <bool kDirty, uint32_t kStride>
void LCDTAP_INLINE ControllerBase::ramwrRgb444RunImpl(const uint8_t* data,
                                                      uint32_t numGroups,
                                                      uint32_t strideArg,
                                                      DirtyAcc& acc) {
  const uint32_t stride = (kStride != 0) ? kStride : strideArg;
  const uint16_t inv = cachedInverter;
  const uint32_t inv32 = (static_cast<uint32_t>(inv) << 16) | inv;
  const int32_t hStep = cachedHStep;
  const bool bgr = cachedBGR;
  const uint8_t* p = data;
  uint32_t numPixels = numGroups * 2u;
  uint32_t x = ramwrX;
  uint16_t* ptr = writePtr;
  // 0x10000 | pixel when a 3-byte group is split by a run boundary (px0 ends
  // the run, px1 opens the next one). The total pixel count is even, so the
  // carry is always consumed before the loop exits.
  uint32_t pending = 0;

  // Decode one 3-byte group into a pixel pair: px0 in [15:0], px1 in [31:16],
  // BGR swap and inversion already applied.
  auto decodeGroup = [&]() [[gnu::always_inline]] {
    uint_fast32_t b0 = p[0];
    uint_fast32_t b1 = p[stride];
    uint_fast32_t b2 = p[stride * 2u];
    p += stride * 3u;
    uint32_t w = ((b0 << 8) & 0xF000u) | ((b0 << 7) & 0x0780u) |
                 ((b1 >> 3) & 0x001Eu) | ((b1 << 28) & 0xF0000000u) |
                 ((b2 << 19) & 0x07800000u) | ((b2 << 17) & 0x001E0000u);
    if (bgr) {
      w = ((w << 11) & 0xF800F800u) | (w & 0x07E007E0u) |
          ((w >> 11) & 0x001F001Fu);
    }
    return w ^ inv32;
  };

  while (numPixels != 0) {
    int32_t rowRemain =
        static_cast<int32_t>(casetXE) - static_cast<int32_t>(x) + 1;
    uint32_t run = rowRemain <= 0 ? 1u : static_cast<uint32_t>(rowRemain);
    if (run > numPixels) run = numPixels;
    if constexpr (kDirty) {
      uint32_t segRemain = 64u - (x & 63u);
      if (run > segRemain) run = segRemain;
    }
    numPixels -= run;
    x += run;

    uint32_t n = run;
    if (pending != 0) {
      uint_fast16_t px = static_cast<uint16_t>(pending);
      if constexpr (kDirty) {
        acc.diff |= static_cast<uint32_t>(*ptr ^ px);
      }
      *ptr = static_cast<uint16_t>(px);
      ptr += hStep;
      pending = 0;
      --n;
    }
    // Unlike RGB565, the pair grouping is fixed by the stream, so a
    // misaligned destination cannot be re-aligned by peeling one pixel;
    // fall back to the scalar loop instead (unaligned 32-bit stores are not
    // portable across every target this builds for).
    if (hStep == 1 && (reinterpret_cast<uintptr_t>(ptr) & 3u) == 0) {
      uint32_t* ptr32 = reinterpret_cast<uint32_t*>(ptr);
      for (uint32_t k = n / 2u; k != 0; --k) {
        uint32_t w = decodeGroup();
        if constexpr (kDirty) {
          acc.diff |= *ptr32 ^ w;
        }
        *ptr32++ = w;
      }
      ptr = reinterpret_cast<uint16_t*>(ptr32);
    } else {
      for (uint32_t k = n / 2u; k != 0; --k) {
        uint32_t w = decodeGroup();
        uint_fast16_t px0 = static_cast<uint16_t>(w);
        uint_fast16_t px1 = w >> 16;
        if constexpr (kDirty) {
          acc.diff |= static_cast<uint32_t>(*ptr ^ px0);
        }
        *ptr = static_cast<uint16_t>(px0);
        ptr += hStep;
        if constexpr (kDirty) {
          acc.diff |= static_cast<uint32_t>(*ptr ^ px1);
        }
        *ptr = static_cast<uint16_t>(px1);
        ptr += hStep;
      }
    }
    if (n & 1u) {
      uint32_t w = decodeGroup();
      uint_fast16_t px0 = static_cast<uint16_t>(w);
      if constexpr (kDirty) {
        acc.diff |= static_cast<uint32_t>(*ptr ^ px0);
      }
      *ptr = static_cast<uint16_t>(px0);
      ptr += hStep;
      pending = 0x10000u | (w >> 16);
    }

    if constexpr (kDirty) {
      if (acc.diff != 0) {
        acc.segBits |= 1u << ((x - 1u) >> 6);
        acc.diff = 0;
      }
    }
    if (x > casetXE) {
      if constexpr (kDirty) {
        if (acc.segBits != 0) {
          markLogicalRowDirty(ramwrY, acc.segBits);
          acc.segBits = 0;
        }
      }
      x = casetXS;
      if (++ramwrY > rasetYE) ramwrY = rasetYS;
      ptr = frameBuffer + physIndex(x, ramwrY);
    }
  }
  ramwrX = static_cast<uint16_t>(x);
  writePtr = ptr;
}

// SRAM-resident on RP2350 (LCDTAP_RAM_FUNC): these loops are the Core 0
// input-path bottleneck at 62.5 MHz SPI, where XIP fetch jitter eats into
// the ring-buffer budget. The always_inline template bodies expand here so
// the hot code actually lands in the SRAM section.
void LCDTAP_RAM_FUNC ControllerBase::ramwrRgb565RunRing(const uint8_t* data,
                                                        uint32_t numPixels,
                                                        DirtyAcc& acc) {
  ramwrRgb565RunImpl<false, sizeof(uint32_t)>(data, numPixels, sizeof(uint32_t),
                                              acc);
}

void LCDTAP_RAM_FUNC ControllerBase::ramwrRgb565RunRingDirty(
    const uint8_t* data, uint32_t numPixels, DirtyAcc& acc) {
  ramwrRgb565RunImpl<true, sizeof(uint32_t)>(data, numPixels, sizeof(uint32_t),
                                             acc);
}

void LCDTAP_RAM_FUNC ControllerBase::ramwrRgb444RunRing(const uint8_t* data,
                                                        uint32_t numGroups,
                                                        DirtyAcc& acc) {
  ramwrRgb444RunImpl<false, sizeof(uint32_t)>(data, numGroups, sizeof(uint32_t),
                                              acc);
}

void LCDTAP_RAM_FUNC ControllerBase::ramwrRgb444RunRingDirty(
    const uint8_t* data, uint32_t numGroups, DirtyAcc& acc) {
  ramwrRgb444RunImpl<true, sizeof(uint32_t)>(data, numGroups, sizeof(uint32_t),
                                             acc);
}

template <bool kDirty>
void ControllerBase::processRamwrDataImpl(const uint8_t* data,
                                          uint32_t numBytes, uint32_t stride) {
  int32_t i = 0;
  int32_t length = numBytes * stride;
  DirtyAcc acc = {0, 0};

  InterfaceFormat effectiveFmt =
      (config.interfaceFormatOverride >= 0)
          ? static_cast<InterfaceFormat>(config.interfaceFormatOverride)
          : interfaceFormat;
  switch (effectiveFmt) {
    case InterfaceFormat::RGB332: {
      // 1 byte = 1 pixel: {R[2:0], G[2:0], B[1:0]}
      // Expand 3→5 bits: (r<<2)|(r>>1)  3→6 bits: (g<<3)|g  2→5 bits:
      // (b<<3)|(b<<1)|(b>>1)
      while (i < length) {
        uint8_t b = data[i];
        i += stride;
        uint_fast16_t r5 = (b >> 5) & 0x07u;
        uint_fast16_t g3 = (b >> 2) & 0x07u;
        uint_fast16_t b2 = b & 0x03u;
        r5 = (r5 << 2) | (r5 >> 1);
        uint_fast16_t g6 = (g3 << 3) | g3;
        uint_fast16_t b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
        writePixelRgb565<kDirty>(
            static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5), acc);
      }
      break;
    }

    case InterfaceFormat::RGB111_HPACK2_H2L_RA8: {
      // 1 byte = 2 pixels: bits[5:3] = younger pixel, bits[2:0] = older pixel
      // 3-bit RGB -> RGB565 lookup (R:1->5bit, G:1->6bit, B:1->5bit)
      static const uint16_t kLut[8] = {
          0x0000, 0x001F, 0x07E0, 0x07FF, 0xF800, 0xF81F, 0xFFE0, 0xFFFF,
      };
      while (i < length) {
        uint8_t b = data[i];
        i += stride;
        writePixelRgb565<kDirty>(kLut[(b >> 3) & 0x07u], acc);
        writePixelRgb565<kDirty>(kLut[b & 0x07u], acc);
      }
      break;
    }

    case InterfaceFormat::RGB444_HPACK2_H2L_BE:
      // byte0: R1[3:0] G1[3:0]  byte1: B1[3:0] R2[3:0]  byte2: G2[3:0] B2[3:0]
      // 4bit→5bit: x<<1 (MSB-align; LSB zeroed)  4bit→6bit: x<<2 (MSB-align;
      // lower 2 bits zeroed) Drain leftover bytes (0–2 bytes)
      while (ramwrBufLen > 0 && i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
        if (ramwrBufLen == 3) {
          uint_fast16_t b0 = ramwrBuf[0], b1 = ramwrBuf[1], b2 = ramwrBuf[2];
          uint_fast16_t pixel0 = 0;
          pixel0 |= (b0 << 8) & 0xF000;  // R1
          pixel0 |= (b0 << 7) & 0x0780;  // G1
          pixel0 |= (b1 >> 3) & 0x001E;  // B1
          writePixelRgb565<kDirty>(pixel0, acc);
          uint_fast16_t pixel1 = 0;
          pixel1 |= (b1 << 12) & 0xF000;  // R2
          pixel1 |= (b2 << 3) & 0x0780;   // G2
          pixel1 |= (b2 << 1) & 0x001E;   // B2
          writePixelRgb565<kDirty>(pixel1, acc);
          ramwrBufLen = 0;
        }
      }
      // Tight loop: 3 bytes → 2 pixels
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
      {
        // cachedBGR-hoisted fast path: writePixelRgb565() re-checks
        // cachedBGR on every call (twice per iteration here). BGR is
        // loop-invariant, so branch on it once and run one of two loop
        // bodies instead. Row wrap is still checked per pixel (a row
        // boundary can fall between pixel0 and pixel1 of the same
        // 3-byte group, independent of the 3-byte grouping), so
        // putPixel() keeps that part of writePixelRgb565()'s logic
        // unchanged, just without the BGR branch (the caller already
        // applied it, or not, depending on which loop below runs).
        // always_inline guarantees this doesn't reintroduce a
        // function-call boundary, same idiom as writePixelRgb565()
        // itself (LCDTAP_INLINE).
        const uint16_t inv = cachedInverter;
        const int32_t hStep = cachedHStep;
        auto putPixel = [&](uint_fast16_t px) [[gnu::always_inline]] {
          px ^= inv;
          if constexpr (kDirty) {
            acc.diff |= static_cast<uint32_t>(*writePtr ^ px);
          }
          *writePtr = (uint16_t)px;
          writePtr += hStep;
          ++ramwrX;
          if constexpr (kDirty) {
            if ((ramwrX & 63u) == 0u) flushDirtySeg(acc);
          }
          if (ramwrX > casetXE) {
            if constexpr (kDirty) flushDirtyRow(acc);
            ramwrX = casetXS;
            if (++ramwrY > rasetYE) ramwrY = rasetYS;
            writePtr = frameBuffer + physIndex(ramwrX, ramwrY);
          }
        };
        if (cachedBGR) {
          while (i + stride * 3 <= length) {
            uint_fast16_t b0 = data[i];
            i += stride;
            uint_fast16_t b1 = data[i];
            i += stride;
            uint_fast16_t b2 = data[i];
            i += stride;
            uint_fast16_t px0 = ((b0 << 8) & 0xF000) | ((b0 << 7) & 0x0780) |
                                ((b1 >> 3) & 0x001E);
            uint_fast16_t px1 = ((b1 << 12) & 0xF000) | ((b2 << 3) & 0x0780) |
                                ((b2 << 1) & 0x001E);
            px0 = (uint_fast16_t)(((px0 << 11) & 0xF800u) | (px0 & 0x07E0u) |
                                  ((px0 >> 11) & 0x1Fu));
            px1 = (uint_fast16_t)(((px1 << 11) & 0xF800u) | (px1 & 0x07E0u) |
                                  ((px1 >> 11) & 0x1Fu));
            putPixel(px0);
            putPixel(px1);
          }
        } else {
          while (i + stride * 3 <= length) {
            uint_fast16_t b0 = data[i];
            i += stride;
            uint_fast16_t b1 = data[i];
            i += stride;
            uint_fast16_t b2 = data[i];
            i += stride;
            uint_fast16_t px0 = ((b0 << 8) & 0xF000) | ((b0 << 7) & 0x0780) |
                                ((b1 >> 3) & 0x001E);
            uint_fast16_t px1 = ((b1 << 12) & 0xF000) | ((b2 << 3) & 0x0780) |
                                ((b2 << 1) & 0x001E);
            putPixel(px0);
            putPixel(px1);
          }
        }
      }
#else
      {
        uint32_t groups = static_cast<uint32_t>(length - i) / (stride * 3u);
        if (groups != 0) {
          if (stride == sizeof(uint32_t)) {
            if constexpr (kDirty) {
              ramwrRgb444RunRingDirty(data + i, groups, acc);
            } else {
              ramwrRgb444RunRing(data + i, groups, acc);
            }
          } else if (stride == 1) {
            ramwrRgb444RunImpl<kDirty, 1>(data + i, groups, stride, acc);
          } else {
            ramwrRgb444RunImpl<kDirty, 0>(data + i, groups, stride, acc);
          }
          i += static_cast<int32_t>(groups * 3u * stride);
        }
      }
#endif
      // Save remainder
      while (i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
      }
      break;

    case InterfaceFormat::RGB565_BE:
      // Drain leftover 1 byte
      if (ramwrBufLen == 1 && i < length) {
        uint_fast16_t b0 = ramwrBuf[0];
        uint_fast16_t b1 = data[i];
        uint_fast16_t pixel =
            cachedLittleEndian ? (b0 | (b1 << 8)) : ((b0 << 8) | b1);
        writePixelRgb565<kDirty>(pixel, acc);
        i += stride;
        ramwrBufLen = 0;
      }
      // Tight loop: 2 bytes → 1 pixel
      {
        uint32_t pixels = static_cast<uint32_t>(length - i) / (stride * 2u);
        if (pixels != 0) {
          if (stride == sizeof(uint32_t)) {
            if constexpr (kDirty) {
              ramwrRgb565RunRingDirty(data + i, pixels, acc);
            } else {
              ramwrRgb565RunRing(data + i, pixels, acc);
            }
          } else if (stride == 1) {
            ramwrRgb565RunImpl<kDirty, 1>(data + i, pixels, stride, acc);
          } else {
            ramwrRgb565RunImpl<kDirty, 0>(data + i, pixels, stride, acc);
          }
          i += static_cast<int32_t>(pixels * 2u * stride);
        }
      }
      // Save remainder
      if (i < length) {
        ramwrBuf[0] = data[i];
        ramwrBufLen = 1;
      }
      break;

    case InterfaceFormat::RGB666_UNPACK_LA8_BE:
      // Upper 6 bits of each byte are significant (lower 2 bits = 0)
      // R5 = byte0>>3, G6 = byte1>>2, B5 = byte2>>3 maps directly to RGB565
      // Drain leftover bytes (0–2 bytes)
      while (ramwrBufLen > 0 && i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
        if (ramwrBufLen == 3) {
          uint_fast16_t pixel = 0;
          pixel |= (ramwrBuf[0] & 0xFCu) << 8;  // R5
          pixel |= (ramwrBuf[1] & 0xFCu) << 3;  // G6
          pixel |= (ramwrBuf[2] & 0xFCu) >> 3;  // B5
          writePixelRgb565<kDirty>(static_cast<uint16_t>(pixel), acc);
          ramwrBufLen = 0;
        }
      }
      // Tight loop: 3 bytes → 1 pixel
      while (i + stride * 3 <= length) {
        uint_fast16_t pixel = 0;
        pixel |= (data[i] & 0xFCu) << 8;  // R5
        i += stride;
        pixel |= (data[i] & 0xFCu) << 3;  // G6
        i += stride;
        pixel |= (data[i] & 0xFCu) >> 3;  // B5
        i += stride;
        writePixelRgb565<kDirty>(static_cast<uint16_t>(pixel), acc);
      }
      // Save remainder
      while (i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
      }
      break;

    case InterfaceFormat::RGB666_UNPACK_RA8_BE:
      // 3 bytes = 1 pixel: color data in bits[5:0] of each byte (right-aligned)
      // R5 = byte0[5:1], G6 = byte1[5:0], B5 = byte2[5:1]
      // (bit[0] of R and B channels is hardware-unused on SSD1331)
      // Drain leftover bytes (0-2 bytes)
      while (ramwrBufLen > 0 && i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
        if (ramwrBufLen == 3) {
          uint_fast16_t pixel = 0;
          pixel |= static_cast<uint_fast16_t>((ramwrBuf[0] >> 1) & 0x1Fu)
                   << 11;                                                 // R5
          pixel |= static_cast<uint_fast16_t>(ramwrBuf[1] & 0x3Fu) << 5;  // G6
          pixel |=
              static_cast<uint_fast16_t>((ramwrBuf[2] >> 1) & 0x1Fu);  // B5
          writePixelRgb565<kDirty>(static_cast<uint16_t>(pixel), acc);
          ramwrBufLen = 0;
        }
      }
      // Tight loop: 3 bytes → 1 pixel
      while (i + stride * 3 <= length) {
        uint_fast16_t pixel = 0;
        pixel |= static_cast<uint_fast16_t>((data[i] >> 1) & 0x1Fu)
                 << 11;  // R5
        i += stride;
        pixel |= static_cast<uint_fast16_t>(data[i] & 0x3Fu) << 5;  // G6
        i += stride;
        pixel |= static_cast<uint_fast16_t>((data[i] >> 1) & 0x1Fu);  // B5
        i += stride;
        writePixelRgb565<kDirty>(static_cast<uint16_t>(pixel), acc);
      }
      // Save remainder
      while (i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
      }
      break;
  }

  // Chunks end at arbitrary byte boundaries, so publish the partial row now;
  // over-marking a partially written row is safe (dirty means "may have
  // changed"), losing the accumulation is not.
  if (kDirty) flushDirtyRow(acc);
}

void ControllerBase::markDirtyRect(uint32_t physRow0, uint32_t physRow1,
                                   uint32_t physCol0, uint32_t physCol1) {
  const uint32_t lastRow = static_cast<uint32_t>(config.buffHeight) - 1u;
  const uint32_t lastCol = static_cast<uint32_t>(config.buffWidth) - 1u;
  if (physRow0 > lastRow || physCol0 > lastCol) return;
  if (physRow1 > lastRow) physRow1 = lastRow;
  if (physCol1 > lastCol) physCol1 = lastCol;
  const uint32_t b0 = physCol0 >> 6;
  const uint32_t b1 = physCol1 >> 6;
  const uint8_t mask =
      static_cast<uint8_t>(((2u << b1) - 1u) & ~((1u << b0) - 1u));
  for (uint32_t r = physRow0; r <= physRow1; ++r) dirtyMap[r] |= mask;
}

// The logical→physical mapping is axis-aligned (physIndex is linear with
// steps of ±1 / ±buffWidth), so mapping the two corners of a rectangle and
// normalizing yields the covering physical rectangle.
void ControllerBase::markLogicalRectDirty(uint32_t lx0, uint32_t lx1,
                                          uint32_t ly0, uint32_t ly1) {
  const uint32_t W = config.buffWidth;
  uint32_t i0 = physIndex(lx0, ly0);
  uint32_t i1 = physIndex(lx1, ly1);
  uint32_t r0 = i0 / W, c0 = i0 % W;
  uint32_t r1 = i1 / W, c1 = i1 % W;
  if (r0 > r1) std::swap(r0, r1);
  if (c0 > c1) std::swap(c0, c1);
  markDirtyRect(r0, r1, c0, c1);
}

void ControllerBase::markLogicalRowDirty(uint32_t ly, uint32_t segBits) {
  for (uint32_t k = 0; segBits != 0; ++k, segBits >>= 1) {
    if (!(segBits & 1u)) continue;
    // Clip the segment to the CASET window: pixels outside it were never
    // written, and physIndex() is only valid for in-range coordinates.
    uint32_t lx0 = k << 6;
    uint32_t lx1 = lx0 + 63u;
    if (lx0 < casetXS) lx0 = casetXS;
    if (lx1 > casetXE) lx1 = casetXE;
    if (lx0 > lx1) continue;
    markLogicalRectDirty(lx0, lx1, ly, ly);
  }
}

void ControllerBase::markAllDirty() {
  const uint32_t rows = config.buffHeight;
  const uint32_t lastSeg = (static_cast<uint32_t>(config.buffWidth) - 1u) >> 6;
  const uint8_t mask = static_cast<uint8_t>((2u << lastSeg) - 1u);
  if (rows > LcdTap::MAX_DIRTY_ROWS) return;
  for (uint32_t r = 0; r < rows; ++r) dirtyMap[r] = mask;
}

// Process a data byte stream: RAMWR data is handled in bulk, others byte by
// byte
void ControllerBase::feedData(const uint8_t* data, uint32_t numBytes,
                              uint32_t stride) {
  if (stride == 0) stride = 1;  // safety guard
  if (isRamWriteCommand()) {
    if (!writeProtected) processRamwrData(data, numBytes, stride);
  } else {
    for (uint32_t i = 0; i < numBytes * stride; i += stride) {
      feedDataByte(data[i]);
    }
  }
}

//=============================================================================
// LcdTap
//=============================================================================

LcdTap::LcdTap(const LcdTapConfig& config, const HostInterface& host)
    : impl_(nullptr) {
  if (!host.alloc || !host.free) return;

  ControllerBase* ctrl = nullptr;
  switch (config.controllerFamily) {
    case ControllerFamily::ST7789:
      ctrl = new (std::nothrow) St7789Controller();
      break;
    case ControllerFamily::SSD1306:
      ctrl = new (std::nothrow) Ssd1306Controller();
      break;
    case ControllerFamily::SSD1331:
      ctrl = new (std::nothrow) Ssd1331Controller();
      break;
    case ControllerFamily::ILI9341:
      ctrl = new (std::nothrow) Ili9341Controller();
      break;
  }
  if (!ctrl) return;

  ctrl->config = config;
  ctrl->host = host;
  ctrl->status = Status::OK;
  ctrl->hwReset = false;
  ctrl->frameBuffer = nullptr;
  ctrl->outputRotation = config.outputRotation & 3u;

  if (config.buffWidth == 0 || config.buffHeight == 0 || config.dviWidth == 0 ||
      config.dviHeight == 0) {
    ctrl->status = Status::INVALID_PARAM;
    impl_ = ctrl;
    return;
  }

  size_t fbSize =
      (size_t)config.buffWidth * config.buffHeight * sizeof(uint16_t);
  ctrl->frameBuffer = static_cast<uint16_t*>(host.alloc(fbSize));
  if (!ctrl->frameBuffer) {
    ctrl->status = Status::OUT_OF_MEMORY;
    impl_ = ctrl;
    return;
  }

  impl_ = ctrl;
  ctrl->calcScaleParams();
  ctrl->softReset();
}

LcdTap::~LcdTap() {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->frameBuffer) ctrl->host.free(ctrl->frameBuffer);
  delete ctrl;
}

Status LcdTap::getStatus() const {
  if (!impl_) return Status::OUT_OF_MEMORY;
  return static_cast<const ControllerBase*>(impl_)->status;
}

void LcdTap::inputReset(bool assert) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK) return;
  // Count assert edges only: the GPIO IRQ path may call inputReset(true)
  // twice for a single physical pulse (EDGE_FALL handler + level re-sample).
  if (assert && !ctrl->hwReset) ++hwResetCount_;
  ctrl->hwReset = assert;
  if (!assert) {
    if (dumpState_ == DumpState::ACTIVE) dumpPush(DUMP_EVENT_HW_RESET);
    ctrl->softReset();
    ctrl->log("HW RESET released");
  }
}

void LcdTap::inputCommand(uint8_t byte) {
  if (!impl_) return;
  ++rxCmdBytes_;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK || ctrl->hwReset) return;
  ctrl->dispatchCommand(byte);
  if (dumpState_ == DumpState::ACTIVE) dumpPush(byte);
}

void LcdTap::inputData(const uint8_t* data, uint32_t numBytes,
                       uint32_t stride) {
  if (!impl_) return;
  rxDataBytes_ += numBytes;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK || ctrl->hwReset) return;
  ctrl->feedData(data, numBytes, stride);
  if (dumpState_ == DumpState::ACTIVE) {
    const uint32_t s = (stride == 0) ? 1u : stride;
    for (uint32_t i = 0; i < numBytes && dumpState_ == DumpState::ACTIVE; ++i)
      dumpPush(0x100u | data[i * s]);
  }
}

uint32_t LcdTap::getUnknownCmdCount() const {
  if (!impl_) return 0;
  return static_cast<const ControllerBase*>(impl_)->unknownCmdCount;
}

uint8_t LcdTap::getLastUnknownCmd() const {
  if (!impl_) return 0;
  return static_cast<const ControllerBase*>(impl_)->lastUnknownCmd;
}

//=============================================================================
// LcdTap dump API
//=============================================================================

void LcdTap::dumpStart(const DumpConfig& dumpCfg) {
  dumpConfig_ = dumpCfg;
  dumpBuffSize_ = 0;
  dumpState_ = DumpState::WAIT;
}

DumpState LcdTap::dumpGetState() const { return dumpState_; }

uint16_t LcdTap::dumpGetSize() const { return dumpBuffSize_; }

void LcdTap::dumpForceTrigger() {
  if (dumpState_ == DumpState::WAIT) dumpState_ = DumpState::ACTIVE;
}

void LcdTap::dumpAbort() { dumpState_ = DumpState::COMPLETE; }

const uint16_t* LcdTap::dumpGetBuffer() const { return dumpBuffer_; }

void LCDTAP_INLINE scaleLine(const uint16_t* src, uint16_t* dest,
                             uint32_t destW, uint32_t hAccum, uint32_t hStep,
                             uint32_t stride) {
  constexpr int UNROLL_SIZE = 8;
  uint32_t* dest32 __attribute__((aligned(4))) =
      reinterpret_cast<uint32_t*>(dest);
  uint32_t destWdiv8 = destW / UNROLL_SIZE;
#ifdef PICO_RP2350
  interp_config cfg = interp_default_config();
  interp_config_set_shift(&cfg, FIXPT_PREC);
  interp_config_set_mask(&cfg, 0, 15);
  interp_config_set_add_raw(&cfg, true);
  interp_set_config(interp0, 0, &cfg);
  interp_set_base(interp0, 0, hStep);
  interp_set_accumulator(interp0, 0, hAccum);
  for (uint32_t x = destWdiv8; x != 0; --x) {
    uint32_t word;
    word = src[interp_pop_full_result(interp0) * stride];
    word |= src[interp_pop_full_result(interp0) * stride] << 16;
    *dest32++ = word;
    word = src[interp_pop_full_result(interp0) * stride];
    word |= src[interp_pop_full_result(interp0) * stride] << 16;
    *dest32++ = word;
    word = src[interp_pop_full_result(interp0) * stride];
    word |= src[interp_pop_full_result(interp0) * stride] << 16;
    *dest32++ = word;
    word = src[interp_pop_full_result(interp0) * stride];
    word |= src[interp_pop_full_result(interp0) * stride] << 16;
    *dest32++ = word;
  }
  dest += destWdiv8 * UNROLL_SIZE;
  for (uint32_t x = destW % UNROLL_SIZE; x != 0; --x) {
    *dest++ = src[interp_pop_full_result(interp0) * stride];
  }
#else
  for (uint32_t x = destWdiv8; x != 0; --x) {
    uint32_t word;
    word = src[(hAccum >> FIXPT_PREC) * stride];
    hAccum += hStep;
    word |= src[(hAccum >> FIXPT_PREC) * stride] << 16;
    hAccum += hStep;
    *dest32++ = word;
    word = src[(hAccum >> FIXPT_PREC) * stride];
    hAccum += hStep;
    word |= src[(hAccum >> FIXPT_PREC) * stride] << 16;
    hAccum += hStep;
    *dest32++ = word;
    word = src[(hAccum >> FIXPT_PREC) * stride];
    hAccum += hStep;
    word |= src[(hAccum >> FIXPT_PREC) * stride] << 16;
    hAccum += hStep;
    *dest32++ = word;
    word = src[(hAccum >> FIXPT_PREC) * stride];
    hAccum += hStep;
    word |= src[(hAccum >> FIXPT_PREC) * stride] << 16;
    hAccum += hStep;
    *dest32++ = word;
  }
  dest += destWdiv8 * UNROLL_SIZE;
  for (uint32_t x = destW % UNROLL_SIZE; x != 0; --x) {
    *dest++ = src[(hAccum >> FIXPT_PREC) * stride];
    hAccum += hStep;
  }
#endif
}

// SRAM-resident (LCDTAP_RAM_FUNC): called per scanline from Core 1, where a
// flash-resident body makes the fill time depend on XIP contention with
// Core 0 (see hot.hpp). scaleLine() is always_inline, so it is carried along.
// The border clears are open-coded loops instead of memset() because memset
// itself lives in flash.
void LCDTAP_RAM_FUNC LcdTap::fillScanline(uint16_t dviLine,
                                          uint16_t* dst) const {
  if (!impl_) return;
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  if (ctrl->status != Status::OK || !ctrl->frameBuffer) return;

  const uint16_t dviW = ctrl->config.dviWidth;

  const uint32_t srcX = ctrl->outSrcX;
  const uint32_t srcY = ctrl->outSrcY;
  const uint32_t srcW = ctrl->outSrcW;
  const uint32_t srcH = ctrl->outSrcH;
  const uint32_t srcR = srcX + srcW - 1;
  const uint32_t srcB = srcY + srcH - 1;
  const uint32_t destX =
      ctrl->outDestX & 0xFFFEu;  // align to even pixel boundary
  const uint32_t destY = ctrl->outDestY;
  const uint32_t destW = ctrl->outDestW;
  const uint32_t destH = ctrl->outDestH;
  const uint32_t stepH = ctrl->outSrcStepH;
  const uint32_t stepV = ctrl->outSrcStepV;

  // Display off, sleeping, or in the vertical black border region
  bool powerOff =
      !ctrl->config.forcePowerOn && (ctrl->sleeping || !ctrl->displayOn);
  if (powerOff || dviLine < destY || dviLine >= destY + destH || srcW == 0 ||
      srcH == 0) {
    for (uint32_t i = 0; i < dviW; ++i) dst[i] = 0;
    return;
  }

  // Vertical mapping: compute LCD row via fixed-point multiply
  const uint32_t lcdRowOut =
      (((uint32_t)(dviLine - destY) * stepV) >> FIXPT_PREC);

  uint16_t* dest = dst;

  // Left black border
  for (uint32_t i = 0; i < destX; ++i) dest[i] = 0;
  dest += destX;

  // Active area: horizontal scaling + brightness inversion + rotation
  // Inverse: XOR all RGB565 bits → (31-R, 63-G, 31-B) inverts each channel
  const bool inverted = ctrl->inverted;
  const uint16_t* fb = ctrl->frameBuffer;
  const uint32_t stride = ctrl->config.buffWidth;

  switch (ctrl->outputRotation) {
    default:
    case 0:
    case 2: {
      // rot=0: fb[(lcdRowOut+srcY)*stride + srcX + col], forward
      // rot=2: fb[(srcB-lcdRowOut)*stride + srcX + col], reverse
      // Reversed start = (destW-1)*stepH so the sample index at output k is
      // exactly the forward index at destW-1-k: the mirrored image is the
      // pixel-exact mirror of the forward one, and span consumers
      // (DisplayLink partial updates) can map mirrored spans by reflecting
      // forward spans without rounding error.
      const bool rev = (ctrl->outputRotation == 2);
      const uint16_t* src = rev ? fb + (srcB - lcdRowOut) * stride + srcX
                                : fb + (lcdRowOut + srcY) * stride + srcX;
      uint32_t hAccum = rev ? (uint32_t)(destW - 1) * stepH : 0;
      const uint32_t hStep = rev ? (uint32_t)(-(int32_t)stepH) : stepH;
      scaleLine(src, dest, destW, hAccum, hStep, 1);
      break;
    }
    case 1:
    case 3: {
      // rot=1: fb[srcY*stride + (lcdRowOut+srcX) + row*stride], rows srcH-1→0
      // rot=3: fb[srcY*stride + (srcR-lcdRowOut)  + row*stride], rows 0→srcH-1
      // Reversed start: same exact-mirror form as rot=2 above.
      const bool rev = (ctrl->outputRotation == 1);
      const uint16_t* src = rev ? fb + srcY * stride + lcdRowOut + srcX
                                : fb + srcY * stride + (srcR - lcdRowOut);
      uint32_t hAccum = rev ? (uint32_t)(destW - 1) * stepH : 0;
      const uint32_t hStep = rev ? (uint32_t)(-(int32_t)stepH) : stepH;
      scaleLine(src, dest, destW, hAccum, hStep, stride);
      break;
    }
  }
  dest += destW;

  // Right black border
  const uint32_t rightW = dviW - destX - destW;
  for (uint32_t i = 0; i < rightW; ++i) dest[i] = 0;
}

LcdTapConfig LcdTap::getConfig() const {
  if (!impl_) return {};
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  LcdTapConfig cfg = ctrl->config;
  cfg.outputRotation = ctrl->outputRotation;
  return cfg;
}

Status LcdTap::updateConfig(const LcdTapConfig& cfg) {
  if (!impl_) return Status::NOT_READY;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK) return ctrl->status;

  if (cfg.buffWidth == 0 || cfg.buffHeight == 0 || cfg.dviWidth == 0 ||
      cfg.dviHeight == 0) {
    return Status::INVALID_PARAM;
  }

  // If the controller type changes, destroy the old controller and create a new
  // one.  The new controller starts from a clean state (sleeping/displayOn are
  // not preserved because the new controller has not yet received any
  // commands).
  if (cfg.controllerFamily != ctrl->config.controllerFamily) {
    HostInterface host = ctrl->host;
    // Carry the dirty-tracking state across the controller swap: the flag is
    // host policy, and the epoch must keep moving forward so a pump that
    // cached the old value still sees a change.
    const bool oldDirtyTracking = ctrl->dirtyTracking;
    const uint32_t oldEpoch = ctrl->presentEpoch;
    // Debug counters must survive the swap: hosts subtract a baseline
    // snapshot from them, so a reset to zero would show as a huge value.
    const uint32_t oldUnknownCmdCount = ctrl->unknownCmdCount;
    const uint8_t oldLastUnknownCmd = ctrl->lastUnknownCmd;
    if (ctrl->frameBuffer) host.free(ctrl->frameBuffer);
    delete ctrl;
    impl_ = nullptr;

    switch (cfg.controllerFamily) {
      case ControllerFamily::ST7789:
        ctrl = new (std::nothrow) St7789Controller();
        break;
      case ControllerFamily::SSD1306:
        ctrl = new (std::nothrow) Ssd1306Controller();
        break;
      case ControllerFamily::SSD1331:
        ctrl = new (std::nothrow) Ssd1331Controller();
        break;
      case ControllerFamily::ILI9341:
        ctrl = new (std::nothrow) Ili9341Controller();
        break;
      default: return Status::OUT_OF_MEMORY;
    }
    if (!ctrl) return Status::OUT_OF_MEMORY;

    ctrl->host = host;
    ctrl->status = Status::OK;
    ctrl->hwReset = false;
    ctrl->frameBuffer = nullptr;
    ctrl->presentEpoch = oldEpoch + 1u;
    ctrl->unknownCmdCount = oldUnknownCmdCount;
    ctrl->lastUnknownCmd = oldLastUnknownCmd;
    impl_ = ctrl;

    size_t fbSize = (size_t)cfg.buffWidth * cfg.buffHeight * sizeof(uint16_t);
    ctrl->frameBuffer = static_cast<uint16_t*>(host.alloc(fbSize));
    if (!ctrl->frameBuffer) return Status::OUT_OF_MEMORY;

    ctrl->config = cfg;
    ctrl->outputRotation = cfg.outputRotation & 3u;
    ctrl->dirtyTracking = oldDirtyTracking &&
                          cfg.buffWidth <= LcdTap::MAX_DIRTY_ROWS &&
                          cfg.buffHeight <= LcdTap::MAX_DIRTY_ROWS;
    if (ctrl->dirtyTracking) ctrl->markAllDirty();
    ctrl->calcScaleParams();
    ctrl->softReset();
    return Status::OK;
  }

  // Same controller type: update config in-place.
  // Allocate new framebuffer before freeing the old one.
  size_t currFbSize = (size_t)ctrl->config.buffWidth * ctrl->config.buffHeight *
                      sizeof(uint16_t);
  size_t newFbSize = (size_t)cfg.buffWidth * cfg.buffHeight * sizeof(uint16_t);
  if (newFbSize > currFbSize) {
    uint16_t* newFb = static_cast<uint16_t*>(ctrl->host.alloc(newFbSize));
    if (!newFb) return Status::OUT_OF_MEMORY;
    ctrl->host.free(ctrl->frameBuffer);
    ctrl->frameBuffer = newFb;
  }

  bool sleeping = ctrl->sleeping;
  bool displayOn = ctrl->displayOn;
  ctrl->config = cfg;
  ctrl->outputRotation = cfg.outputRotation & 3u;
  ctrl->dirtyTracking = ctrl->dirtyTracking &&
                        cfg.buffWidth <= LcdTap::MAX_DIRTY_ROWS &&
                        cfg.buffHeight <= LcdTap::MAX_DIRTY_ROWS;
  if (ctrl->dirtyTracking) ctrl->markAllDirty();
  ctrl->calcScaleParams();
  ctrl->softReset();
  ctrl->sleeping = sleeping;
  ctrl->displayOn = displayOn;
  return Status::OK;
}

// SRAM-resident: the fill callback of every example queries this per line.
void LCDTAP_RAM_FUNC LcdTap::getOutputScreenSize(uint16_t* width,
                                                 uint16_t* height) const {
  if (!impl_) {
    *width = *height = 0;
    return;
  }
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  *width = ctrl->config.dviWidth;
  *height = ctrl->config.dviHeight;
}

bool LcdTap::isOutputInverted() const {
  if (!impl_) return false;
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  return ctrl->inverted;
}

bool LcdTap::isOutputSwapRB() const {
  if (!impl_) return false;
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  return ctrl->cachedBGR;
}

uint16_t* LcdTap::getFramebuf() {
  if (!impl_) return nullptr;
  return static_cast<ControllerBase*>(impl_)->frameBuffer;
}

void LcdTap::getOutSrcRegion(uint16_t* x, uint16_t* y, uint16_t* w,
                             uint16_t* h) const {
  if (!impl_) {
    *x = *y = *w = *h = 0;
    return;
  }
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  *x = ctrl->outSrcX;
  *y = ctrl->outSrcY;
  *w = ctrl->outSrcW;
  *h = ctrl->outSrcH;
}

void LcdTap::setWriteProtected(bool protect) {
  if (!impl_) return;
  static_cast<ControllerBase*>(impl_)->writeProtected = protect;
}

bool LcdTap::isWriteProtected() const {
  if (!impl_) return false;
  return static_cast<const ControllerBase*>(impl_)->writeProtected;
}

void LcdTap::setDisplayOn(bool on) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK) return;
  ctrl->sleeping = !on;
  ctrl->displayOn = on;
  ctrl->bumpEpoch();
}

void LcdTap::setDirtyTracking(bool on) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  ctrl->dirtyTracking = on && ctrl->config.buffWidth <= MAX_DIRTY_ROWS &&
                        ctrl->config.buffHeight <= MAX_DIRTY_ROWS;
  if (ctrl->dirtyTracking) ctrl->markAllDirty();
}

bool LcdTap::isDirtyTrackingActive() const {
  if (!impl_) return false;
  return static_cast<const ControllerBase*>(impl_)->dirtyTracking;
}

uint8_t* LcdTap::dirtyMap() {
  if (!impl_) return nullptr;
  return static_cast<ControllerBase*>(impl_)->dirtyMap;
}

void LcdTap::markAllDirty() {
  if (!impl_) return;
  static_cast<ControllerBase*>(impl_)->markAllDirty();
}

uint32_t LcdTap::getPresentationEpoch() const {
  if (!impl_) return 0;
  return static_cast<const ControllerBase*>(impl_)->presentEpoch;
}

void LcdTap::getOutputMapInfo(OutputMapInfo* out) const {
  *out = OutputMapInfo{};
  if (!impl_) {
    out->blanked = true;
    return;
  }
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  out->destX = ctrl->outDestX & 0xFFFEu;  // fillScanline aligns to even
  out->destY = ctrl->outDestY;
  out->destW = ctrl->outDestW;
  out->destH = ctrl->outDestH;
  out->srcX = ctrl->outSrcX;
  out->srcY = ctrl->outSrcY;
  out->srcW = ctrl->outSrcW;
  out->srcH = ctrl->outSrcH;
  out->stepH = ctrl->outSrcStepH;
  out->stepV = ctrl->outSrcStepV;
  out->rotation = ctrl->outputRotation;
  out->fbWidth = ctrl->config.buffWidth;
  out->fbHeight = ctrl->config.buffHeight;
  const bool powerOff =
      !ctrl->config.forcePowerOn && (ctrl->sleeping || !ctrl->displayOn);
  out->blanked = powerOff || ctrl->status != Status::OK ||
                 ctrl->frameBuffer == nullptr || ctrl->outSrcW == 0 ||
                 ctrl->outSrcH == 0;
}

void LcdTap::setOutputRotation(int rot) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK) return;
  ctrl->config.outputRotation = rot & 3u;
  ctrl->outputRotation = ctrl->config.outputRotation;
  ctrl->calcScaleParams();
}

}  // namespace lcdtap
