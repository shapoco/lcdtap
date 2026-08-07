// Host-side unit test for the test rig's pattern generator
// (../src/testgen.cpp, no MCU dependencies).
//
// Decodes the wire streams produced by buildWireRect() with independent
// per-format reference decoders (written from the interface format
// definitions, not from the generator code) and checks the decoded pixels
// match expectedRgb565() under compareMask(). Also pins determinism,
// windowed generation, solid rows and the text pattern.
//
// Build & run:
//   g++ -O2 -Wall -Wextra -I../include -I../../../lib/include \
//       -o /tmp/testrig_testgen_test testgen_test.cpp ../src/testgen.cpp
//   /tmp/testrig_testgen_test

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "testrig/testgen.hpp"

using namespace testrig;
using lcdtap::InterfaceFormat;

static int gFailures = 0;

#define CHECK(cond, ...)                            \
  do {                                              \
    if (!(cond)) {                                  \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                          \
      printf("\n");                                 \
      gFailures++;                                  \
    }                                               \
  } while (0)

// Reference decoder: wire bytes for a w x rows window -> RGB565 pixels in
// row-major window order (GRAY1: full pages).
static std::vector<uint16_t> refDecode(InterfaceFormat fmt,
                                       const std::vector<uint8_t>& wire,
                                       uint16_t w, uint16_t rows) {
  std::vector<uint16_t> px;
  px.reserve(static_cast<size_t>(w) * rows);
  switch (fmt) {
    case InterfaceFormat::GRAY1_VPACK8_H2L: {
      // Column-major bytes per 8-row page, bit0 = top.
      px.assign(static_cast<size_t>(w) * rows, 0);
      size_t i = 0;
      for (uint16_t page = 0; page < rows / 8; page++) {
        for (uint16_t x = 0; x < w; x++) {
          uint8_t b = wire[i++];
          for (uint16_t bit = 0; bit < 8; bit++) {
            px[static_cast<size_t>(page * 8 + bit) * w + x] =
                (b & (1u << bit)) ? 0xFFFFu : 0x0000u;
          }
        }
      }
      break;
    }
    case InterfaceFormat::RGB111_HPACK2_H2L_RA8:
      for (uint8_t b : wire) {
        for (int k = 1; k >= 0; k--) {
          uint8_t v = (b >> (3 * k)) & 0x07u;
          uint16_t r = (v & 4) ? 0xF800u : 0;
          uint16_t g = (v & 2) ? 0x07E0u : 0;
          uint16_t bl = (v & 1) ? 0x001Fu : 0;
          px.push_back(static_cast<uint16_t>(r | g | bl));
        }
      }
      break;
    case InterfaceFormat::RGB332:
      for (uint8_t b : wire) {
        uint16_t r = static_cast<uint16_t>((b >> 5) & 0x07u);
        uint16_t g = static_cast<uint16_t>((b >> 2) & 0x07u);
        uint16_t bl = static_cast<uint16_t>(b & 0x03u);
        px.push_back(static_cast<uint16_t>((r << 13) | (g << 8) | (bl << 3)));
      }
      break;
    case InterfaceFormat::RGB444_HPACK2_H2L_BE:
      for (size_t i = 0; i + 3 <= wire.size(); i += 3) {
        uint16_t b0 = wire[i], b1 = wire[i + 1], b2 = wire[i + 2];
        px.push_back(static_cast<uint16_t>(
            ((b0 & 0xF0u) << 8) | ((b0 & 0x0Fu) << 7) | ((b1 & 0xF0u) >> 3)));
        px.push_back(static_cast<uint16_t>(
            ((b1 & 0x0Fu) << 12) | ((b2 & 0xF0u) << 3) | ((b2 & 0x0Fu) << 1)));
      }
      break;
    case InterfaceFormat::RGB565_BE:
      for (size_t i = 0; i + 2 <= wire.size(); i += 2) {
        px.push_back(static_cast<uint16_t>((wire[i] << 8) | wire[i + 1]));
      }
      break;
    case InterfaceFormat::RGB666_UNPACK_LA8_BE:
      for (size_t i = 0; i + 3 <= wire.size(); i += 3) {
        uint16_t r5 = static_cast<uint16_t>(wire[i] >> 3);
        uint16_t g6 = static_cast<uint16_t>(wire[i + 1] >> 2);
        uint16_t b5 = static_cast<uint16_t>(wire[i + 2] >> 3);
        px.push_back(static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5));
      }
      break;
    case InterfaceFormat::RGB666_UNPACK_RA8_BE:
      for (size_t i = 0; i + 3 <= wire.size(); i += 3) {
        uint16_t r5 = static_cast<uint16_t>((wire[i] >> 1) & 0x1Fu);
        uint16_t g6 = static_cast<uint16_t>(wire[i + 1] & 0x3Fu);
        uint16_t b5 = static_cast<uint16_t>((wire[i + 2] >> 1) & 0x1Fu);
        px.push_back(static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5));
      }
      break;
    default: break;
  }
  return px;
}

static const InterfaceFormat kAllFormats[] = {
    InterfaceFormat::GRAY1_VPACK8_H2L,
    InterfaceFormat::RGB111_HPACK2_H2L_RA8,
    InterfaceFormat::RGB332,
    InterfaceFormat::RGB444_HPACK2_H2L_BE,
    InterfaceFormat::RGB565_BE,
    InterfaceFormat::RGB666_UNPACK_LA8_BE,
    InterfaceFormat::RGB666_UNPACK_RA8_BE,
};

static void testEncodersMatchExpected() {
  printf("testEncodersMatchExpected\n");
  for (InterfaceFormat fmt : kAllFormats) {
    PatternParams p{fmt, 64, 0x1234u};
    uint16_t w = 32, rows = 16;
    std::vector<uint8_t> wire(wireBytesForRect(fmt, w, rows));
    size_t n = buildWireRect(p, 0, 0, w, rows, wire.data());
    CHECK(n == wire.size(), "fmt %d: size %zu != %zu", (int)fmt, n,
          wire.size());
    std::vector<uint16_t> px = refDecode(fmt, wire, w, rows);
    CHECK(px.size() == static_cast<size_t>(w) * rows, "fmt %d: pixel count",
          (int)fmt);
    uint16_t mask = compareMask(fmt, false);
    for (uint16_t y = 0; y < rows; y++) {
      for (uint16_t x = 0; x < w; x++) {
        uint32_t idx = static_cast<uint32_t>(y) * p.physW + x;
        uint16_t want = expectedRgb565(p, idx, false);
        uint16_t got = px[static_cast<size_t>(y) * w + x];
        if (((got ^ want) & mask) != 0) {
          CHECK(false, "fmt %d (%u,%u): got %04X want %04X mask %04X", (int)fmt,
                x, y, got, want, mask);
          y = rows;  // one failure per format is enough
          break;
        }
      }
    }
  }
}

static void testWindowedGeneration() {
  printf("testWindowedGeneration\n");
  // A windowed build must produce the same pattern values as the same
  // region of a full-frame build (physical-coordinate indexing).
  PatternParams p{InterfaceFormat::RGB565_BE, 40, 99u};
  uint16_t x0 = 6, y0 = 4, w = 10, rows = 8;
  std::vector<uint8_t> wire(wireBytesForRect(p.format, w, rows));
  buildWireRect(p, x0, y0, w, rows, wire.data());
  std::vector<uint16_t> px = refDecode(p.format, wire, w, rows);
  for (uint16_t y = 0; y < rows; y++) {
    for (uint16_t x = 0; x < w; x++) {
      uint32_t idx = static_cast<uint32_t>(y0 + y) * p.physW + (x0 + x);
      CHECK(px[static_cast<size_t>(y) * w + x] == expectedRgb565(p, idx, false),
            "window (%u,%u)", x, y);
    }
  }
}

static void testDeterminismAndSeed() {
  printf("testDeterminismAndSeed\n");
  PatternParams p{InterfaceFormat::RGB332, 32, 7u};
  std::vector<uint8_t> a(wireBytesForRect(p.format, 32, 4));
  std::vector<uint8_t> b(a.size());
  buildWireRect(p, 0, 0, 32, 4, a.data());
  buildWireRect(p, 0, 0, 32, 4, b.data());
  CHECK(a == b, "same seed must be reproducible");
  PatternParams q = p;
  q.seed = 8u;
  buildWireRect(q, 0, 0, 32, 4, b.data());
  CHECK(a != b, "different seed must differ");
}

static void testRbSwapMask() {
  printf("testRbSwapMask\n");
  // RGB332 is R/B-asymmetric: swapping must move the 2-bit blue mask into
  // the red field.
  uint16_t m0 = compareMask(InterfaceFormat::RGB332, false);
  uint16_t m1 = compareMask(InterfaceFormat::RGB332, true);
  CHECK(m0 == 0xE718u, "RGB332 mask %04X", m0);
  CHECK(m1 == 0xC71Cu, "RGB332 swapped mask %04X", m1);
  // Expected value swap moves channels between fields.
  PatternParams p{InterfaceFormat::RGB565_BE, 16, 3u};
  uint16_t a = expectedRgb565(p, 5, false);
  uint16_t b = expectedRgb565(p, 5, true);
  CHECK(((a >> 11) & 0x1F) == (b & 0x1F) && (a & 0x1F) == ((b >> 11) & 0x1F) &&
            (a & 0x07E0) == (b & 0x07E0),
        "rbSwap field swap");
}

static void testSolidRow() {
  printf("testSolidRow\n");
  for (InterfaceFormat fmt : kAllFormats) {
    uint8_t buf[256];
    size_t n = buildSolidRow(fmt, 32, buf);
    CHECK(n == wireBytesPerRow(fmt, 32), "fmt %d solid size", (int)fmt);
  }
}

static void testTextPattern() {
  printf("testTextPattern\n");
  char a[40], b[40];
  buildTextRow(1, 0, 40, a);
  buildTextRow(1, 0, 40, b);
  CHECK(memcmp(a, b, 40) == 0, "text determinism");
  buildTextRow(1, 1, 40, b);
  CHECK(memcmp(a, b, 40) != 0, "text rows must differ");
  for (int i = 0; i < 40; i++) {
    CHECK(a[i] >= 0x21 && a[i] <= 0x7E, "printable char %02X", a[i]);
  }
}

int main() {
  testEncodersMatchExpected();
  testWindowedGeneration();
  testDeterminismAndSeed();
  testRbSwapMask();
  testSolidRow();
  testTextPattern();
  if (gFailures == 0) {
    printf("ALL TESTS PASSED\n");
    return 0;
  }
  printf("%d FAILURE(S)\n", gFailures);
  return 1;
}
