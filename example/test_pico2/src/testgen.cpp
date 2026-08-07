#include "testrig/testgen.hpp"

namespace testrig {

using lcdtap::InterfaceFormat;

size_t wireBytesPerRow(InterfaceFormat fmt, uint16_t w) {
  switch (fmt) {
    case InterfaceFormat::GRAY1_VPACK8_H2L:
      return w;  // one page: 1 byte per column
    case InterfaceFormat::RGB111_HPACK2_H2L_RA8:
      return static_cast<size_t>(w) / 2u;
    case InterfaceFormat::RGB332: return w;
    case InterfaceFormat::RGB444_HPACK2_H2L_BE:
      return static_cast<size_t>(w) * 3u / 2u;
    case InterfaceFormat::RGB565_BE: return static_cast<size_t>(w) * 2u;
    case InterfaceFormat::RGB666_UNPACK_LA8_BE:
    case InterfaceFormat::RGB666_UNPACK_RA8_BE:
      return static_cast<size_t>(w) * 3u;
    default: return 0;
  }
}

size_t wireBytesForRect(InterfaceFormat fmt, uint16_t w, uint16_t rows) {
  size_t perRow = wireBytesPerRow(fmt, w);
  if (fmt == InterfaceFormat::GRAY1_VPACK8_H2L) {
    return perRow * (rows / 8u);
  }
  return perRow * rows;
}

size_t buildWireRect(const PatternParams& p, uint16_t x0, uint16_t y0,
                     uint16_t w, uint16_t rows, uint8_t* out) {
  uint8_t* o = out;

  if (p.format == InterfaceFormat::GRAY1_VPACK8_H2L) {
    // Horizontal addressing order: column-major within a page, then next
    // page. bit0 = top row of the page.
    for (uint16_t page = 0; page < rows / 8u; page++) {
      uint16_t yBase = static_cast<uint16_t>(y0 + page * 8u);
      for (uint16_t x = 0; x < w; x++) {
        uint8_t byte = 0;
        for (uint16_t bit = 0; bit < 8u; bit++) {
          uint32_t idx =
              static_cast<uint32_t>(yBase + bit) * p.physW + (x0 + x);
          if (canonicalGray1(p.seed, idx)) byte |= (1u << bit);
        }
        *o++ = byte;
      }
    }
    return static_cast<size_t>(o - out);
  }

  for (uint16_t row = 0; row < rows; row++) {
    uint32_t base = static_cast<uint32_t>(y0 + row) * p.physW + x0;
    switch (p.format) {
      case InterfaceFormat::RGB111_HPACK2_H2L_RA8:
        // 1 byte = 2 pixels, right-aligned: bits[5:3] = first pixel,
        // bits[2:0] = second pixel, each {R, G, B} high to low.
        for (uint16_t x = 0; x < w; x += 2) {
          Rgb888 c0 = canonicalColor(p.seed, base + x);
          Rgb888 c1 = canonicalColor(p.seed, base + x + 1);
          uint8_t p0 = static_cast<uint8_t>(((c0.r >> 7) << 2) |
                                            ((c0.g >> 7) << 1) | (c0.b >> 7));
          uint8_t p1 = static_cast<uint8_t>(((c1.r >> 7) << 2) |
                                            ((c1.g >> 7) << 1) | (c1.b >> 7));
          *o++ = static_cast<uint8_t>((p0 << 3) | p1);
        }
        break;

      case InterfaceFormat::RGB332:
        for (uint16_t x = 0; x < w; x++) {
          Rgb888 c = canonicalColor(p.seed, base + x);
          *o++ = static_cast<uint8_t>((c.r & 0xE0u) | ((c.g >> 3) & 0x1Cu) |
                                      (c.b >> 6));
        }
        break;

      case InterfaceFormat::RGB444_HPACK2_H2L_BE:
        // 3 bytes = 2 pixels: R1G1, B1R2, G2B2 (each nibble = channel top 4).
        for (uint16_t x = 0; x < w; x += 2) {
          Rgb888 c0 = canonicalColor(p.seed, base + x);
          Rgb888 c1 = canonicalColor(p.seed, base + x + 1);
          *o++ = static_cast<uint8_t>((c0.r & 0xF0u) | (c0.g >> 4));
          *o++ = static_cast<uint8_t>((c0.b & 0xF0u) | (c1.r >> 4));
          *o++ = static_cast<uint8_t>((c1.g & 0xF0u) | (c1.b >> 4));
        }
        break;

      case InterfaceFormat::RGB565_BE:
        for (uint16_t x = 0; x < w; x++) {
          Rgb888 c = canonicalColor(p.seed, base + x);
          uint16_t px = static_cast<uint16_t>(((c.r >> 3) << 11) |
                                              ((c.g >> 2) << 5) | (c.b >> 3));
          *o++ = static_cast<uint8_t>(px >> 8);
          *o++ = static_cast<uint8_t>(px);
        }
        break;

      case InterfaceFormat::RGB666_UNPACK_LA8_BE:
        // Channels left-aligned in each byte (bits[7:2]).
        for (uint16_t x = 0; x < w; x++) {
          Rgb888 c = canonicalColor(p.seed, base + x);
          *o++ = static_cast<uint8_t>(c.r & 0xFCu);
          *o++ = static_cast<uint8_t>(c.g & 0xFCu);
          *o++ = static_cast<uint8_t>(c.b & 0xFCu);
        }
        break;

      case InterfaceFormat::RGB666_UNPACK_RA8_BE:
        // Channels right-aligned in each byte (bits[5:0]).
        for (uint16_t x = 0; x < w; x++) {
          Rgb888 c = canonicalColor(p.seed, base + x);
          *o++ = static_cast<uint8_t>(c.r >> 2);
          *o++ = static_cast<uint8_t>(c.g >> 2);
          *o++ = static_cast<uint8_t>(c.b >> 2);
        }
        break;

      default: return 0;
    }
  }
  return static_cast<size_t>(o - out);
}

size_t buildSolidRow(InterfaceFormat fmt, uint16_t w, uint8_t* out) {
  // Mid-gray keeps every channel active without saturating anything.
  static constexpr uint8_t LEVEL = 0x55;
  size_t n = wireBytesPerRow(fmt, w);
  switch (fmt) {
    case InterfaceFormat::GRAY1_VPACK8_H2L:
      for (size_t i = 0; i < n; i++) out[i] = 0x55;  // dotted pattern
      break;
    case InterfaceFormat::RGB111_HPACK2_H2L_RA8:
      for (size_t i = 0; i < n; i++) out[i] = 0x3F;  // both pixels white
      break;
    case InterfaceFormat::RGB332:
      for (size_t i = 0; i < n; i++) out[i] = 0x52;  // ~mid gray
      break;
    case InterfaceFormat::RGB444_HPACK2_H2L_BE:
      for (size_t i = 0; i < n; i++) out[i] = LEVEL;
      break;
    case InterfaceFormat::RGB565_BE: {
      uint16_t px = static_cast<uint16_t>(((LEVEL >> 3) << 11) |
                                          ((LEVEL >> 2) << 5) | (LEVEL >> 3));
      for (uint16_t x = 0; x < w; x++) {
        out[2 * x] = static_cast<uint8_t>(px >> 8);
        out[2 * x + 1] = static_cast<uint8_t>(px);
      }
      break;
    }
    case InterfaceFormat::RGB666_UNPACK_LA8_BE:
      for (size_t i = 0; i < n; i++) out[i] = LEVEL & 0xFCu;
      break;
    case InterfaceFormat::RGB666_UNPACK_RA8_BE:
      for (size_t i = 0; i < n; i++) out[i] = LEVEL >> 2;
      break;
    default: return 0;
  }
  return n;
}

// Swap the R and B fields of an RGB565 value (mirrors the target's BGR path).
static inline uint16_t swapRb565(uint16_t px) {
  return static_cast<uint16_t>(((px << 11) & 0xF800u) | (px & 0x07E0u) |
                               ((px >> 11) & 0x001Fu));
}

uint16_t expectedRgb565(const PatternParams& p, uint32_t physIndex,
                        bool rbSwap) {
  if (p.format == InterfaceFormat::GRAY1_VPACK8_H2L) {
    return canonicalGray1(p.seed, physIndex) ? 0xFFFFu : 0x0000u;
  }
  Rgb888 c = canonicalColor(p.seed, physIndex);
  uint16_t px = static_cast<uint16_t>(((c.r >> 3) << 11) | ((c.g >> 2) << 5) |
                                      (c.b >> 3));
  return rbSwap ? swapRb565(px) : px;
}

uint16_t compareMask(InterfaceFormat fmt, bool rbSwap) {
  // Significant wire bits per channel.
  uint8_t nr, ng, nb;
  switch (fmt) {
    case InterfaceFormat::GRAY1_VPACK8_H2L:
      return 0xFFFFu;  // pixels are exactly 0x0000 or 0xFFFF
    case InterfaceFormat::RGB111_HPACK2_H2L_RA8: nr = 1, ng = 1, nb = 1; break;
    case InterfaceFormat::RGB332: nr = 3, ng = 3, nb = 2; break;
    case InterfaceFormat::RGB444_HPACK2_H2L_BE: nr = 4, ng = 4, nb = 4; break;
    case InterfaceFormat::RGB565_BE: nr = 5, ng = 6, nb = 5; break;
    case InterfaceFormat::RGB666_UNPACK_LA8_BE:
    case InterfaceFormat::RGB666_UNPACK_RA8_BE:
      nr = 6, ng = 6, nb = 6;  // framebuffer keeps 5/6/5 of these
      break;
    default: return 0;
  }
  // The target's BGR path swaps whole 5-bit fields, so the significant bit
  // counts swap with the channels.
  if (rbSwap) {
    uint8_t t = nr;
    nr = nb;
    nb = t;
  }
  if (nr > 5) nr = 5;
  if (ng > 6) ng = 6;
  if (nb > 5) nb = 5;
  uint16_t mr = static_cast<uint16_t>((0x1Fu << (5 - nr)) & 0x1Fu);
  uint16_t mg = static_cast<uint16_t>((0x3Fu << (6 - ng)) & 0x3Fu);
  uint16_t mb = static_cast<uint16_t>((0x1Fu << (5 - nb)) & 0x1Fu);
  return static_cast<uint16_t>((mr << 11) | (mg << 5) | mb);
}

void buildTextRow(uint32_t seed, uint16_t row, uint16_t cols, char* out) {
  for (uint16_t col = 0; col < cols; col++) {
    uint32_t h = hash32(seed ^ (static_cast<uint32_t>(row) * 251u + col + 1u));
    out[col] = static_cast<char>(0x21 + (h % 0x5Eu));  // 0x21..0x7E
  }
}

}  // namespace testrig
