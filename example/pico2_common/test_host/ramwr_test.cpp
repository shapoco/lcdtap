// Host-side unit test for the RAMWR pixel-write path in lib/ (no MCU
// dependencies).
//
// Feeds RGB565_BE / RGB444_HPACK2_H2L_BE RAMWR streams through the public
// LcdTap API and compares the resulting framebuffer against an independent
// per-pixel reference model, across MADCTL orientations, BGR, endianness,
// inversion, partial/odd/unaligned windows, window wrap-around, chunked
// delivery and input strides (1 = packed, 4 = ring words, 3 = generic).
// This pins down the run-length fast path: run splitting, the 2-pixel
// paired stores, the RGB444 pending-pixel carry and the leftover-byte
// buffering must all be invisible in the output.
//
// Build & run:
//   g++ -O2 -Wall -Wextra -I../../../lib/include -o /tmp/lcdtap_ramwr_test
//       ramwr_test.cpp ../../../lib/src/lcdtap.cpp
//       ../../../lib/src/config.cpp ../../../lib/src/spi_display_base.cpp
//       ../../../lib/src/st7789_controller.cpp
//       ../../../lib/src/ili9341_controller.cpp
//       ../../../lib/src/ssd1306_controller.cpp
//       ../../../lib/src/ssd1331_controller.cpp
//       ../../../lib/src/st7032_controller.cpp
//   /tmp/lcdtap_ramwr_test

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "lcdtap/lcdtap.hpp"

using namespace lcdtap;

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

namespace {

void* testAlloc(size_t size) { return malloc(size); }
void testFree(void* ptr) { free(ptr); }

constexpr uint8_t MADCTL_MY = 0x80;
constexpr uint8_t MADCTL_MX = 0x40;
constexpr uint8_t MADCTL_MV = 0x20;
constexpr uint8_t MADCTL_BGR = 0x08;

// Independent reference model of the ST7789 RAMWR write path: hardware
// window + MADCTL address mapping + per-pixel decode, written as plain
// per-pixel code with none of the run/pair/pending machinery under test.
struct RefModel {
  uint16_t W, H;
  std::vector<uint16_t> fb;
  uint8_t madctl = 0;
  bool le = false;
  bool rgb444 = false;
  uint16_t inverter = 0;
  uint16_t hwColS, hwColE, hwRowS, hwRowE;

  RefModel(uint16_t w, uint16_t h) : W(w), H(h), fb((size_t)w * h, 0) {
    hwColS = 0;
    hwColE = w - 1;
    hwRowS = 0;
    hwRowE = h - 1;
  }

  void ramwr(const std::vector<uint8_t>& bytes) {
    const bool mv = madctl & MADCTL_MV;
    const bool mx = madctl & MADCTL_MX;
    const bool my = madctl & MADCTL_MY;
    const bool bgr = madctl & MADCTL_BGR;
    // CASET always drives the logical (fast) axis and RASET the slow axis;
    // MV only changes which physical axis they map to (issue #0009).
    const uint16_t xs = hwColS;
    const uint16_t xe = hwColE;
    const uint16_t ys = hwRowS;
    const uint16_t ye = hwRowE;
    int32_t hOff, hStep, vOff, vStep;
    if (!mv) {
      hOff = mx ? (W - 1) : 0;
      hStep = mx ? -1 : +1;
      vOff = my ? (H - 1) * W : 0;
      vStep = my ? -W : +W;
    } else {
      hOff = my ? (H - 1) * W : 0;
      hStep = my ? -W : +W;
      vOff = mx ? (W - 1) : 0;
      vStep = mx ? -1 : +1;
    }
    uint32_t x = xs, y = ys;

    auto putPixel = [&](uint16_t px) {
      if (bgr) {
        px = (uint16_t)(((px << 11) & 0xF800u) | (px & 0x07E0u) |
                        ((px >> 11) & 0x1Fu));
      }
      px ^= inverter;
      fb[(size_t)(x * hStep + hOff + y * vStep + vOff)] = px;
      if (++x > xe) {
        x = xs;
        if (++y > ye) y = ys;
      }
    };

    if (!rgb444) {
      for (size_t i = 0; i + 2 <= bytes.size(); i += 2) {
        uint16_t b0 = bytes[i], b1 = bytes[i + 1];
        putPixel(le ? (uint16_t)(b0 | (b1 << 8)) : (uint16_t)((b0 << 8) | b1));
      }
    } else {
      for (size_t i = 0; i + 3 <= bytes.size(); i += 3) {
        uint16_t b0 = bytes[i], b1 = bytes[i + 1], b2 = bytes[i + 2];
        putPixel((uint16_t)(((b0 << 8) & 0xF000) | ((b0 << 7) & 0x0780) |
                            ((b1 >> 3) & 0x001E)));
        putPixel((uint16_t)(((b1 << 12) & 0xF000) | ((b2 << 3) & 0x0780) |
                            ((b2 << 1) & 0x001E)));
      }
    }
  }
};

struct Harness {
  LcdTap tap;
  uint16_t fbW, fbH;

  explicit Harness(const LcdTapConfig& cfg)
      : tap(cfg, HostInterface{testAlloc, testFree, nullptr, nullptr}) {
    fbW = cfg.buffWidth;
    fbH = cfg.buffHeight;
    if (tap.getStatus() != Status::OK) {
      printf("  FATAL: LcdTap init failed\n");
      exit(1);
    }
    memset(tap.getFramebuf(), 0, (size_t)fbW * fbH * sizeof(uint16_t));
  }

  void cmd(uint8_t c) { tap.inputCommand(c); }

  // Delivers bytes in `chunk`-byte pieces, each expanded to the given input
  // stride (extra bytes filled with garbage to catch stride handling bugs).
  void dat(const std::vector<uint8_t>& bytes, uint32_t chunk, uint32_t stride) {
    if (chunk == 0) chunk = (uint32_t)bytes.size();
    std::vector<uint8_t> strided;
    for (size_t i = 0; i < bytes.size(); i += chunk) {
      uint32_t n =
          (uint32_t)((bytes.size() - i < chunk) ? bytes.size() - i : chunk);
      if (stride == 1) {
        tap.inputData(bytes.data() + i, n, 1);
      } else {
        strided.assign((size_t)n * stride, 0xA5);
        for (uint32_t k = 0; k < n; ++k) strided[k * stride] = bytes[i + k];
        tap.inputData(strided.data(), n, stride);
      }
    }
  }

  void param16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
  }

  void setWindow(uint16_t cs, uint16_t ce, uint16_t rs, uint16_t re) {
    std::vector<uint8_t> v;
    cmd(0x2A);  // CASET
    param16(v, cs);
    param16(v, ce);
    dat(v, 0, 1);
    v.clear();
    cmd(0x2B);  // RASET
    param16(v, rs);
    param16(v, re);
    dat(v, 0, 1);
  }
};

std::vector<uint8_t> randomBytes(size_t n, uint32_t seed) {
  std::vector<uint8_t> v(n);
  uint32_t s = seed * 2654435761u + 1u;
  for (size_t i = 0; i < n; ++i) {
    s = s * 1664525u + 1013904223u;
    v[i] = (uint8_t)(s >> 24);
  }
  return v;
}

LcdTapConfig st7789Config(uint16_t w, uint16_t h) {
  LcdTapConfig cfg;
  getDefaultConfig(ControllerFamily::ST7789, &cfg);
  cfg.buffWidth = w;
  cfg.buffHeight = h;
  cfg.dviWidth = 640;
  cfg.dviHeight = 480;
  return cfg;
}

struct Scenario {
  const char* name;
  uint16_t bufW, bufH;
  uint8_t madctl;
  bool rgb444;
  bool le;      // RAMCTRL little-endian (RGB565 only)
  bool invoff;  // send INVOFF (ST7789 default config.inverted=true →
                // inverter becomes 0xFFFF)
  // hardware window (pre-MADCTL coordinates)
  uint16_t cs, ce, rs, re;
  size_t numBytes;  // RAMWR payload length (may leave leftover bytes)
};

void runScenario(const Scenario& sc, uint32_t chunk, uint32_t stride,
                 bool dirtyTracking) {
  Harness h(st7789Config(sc.bufW, sc.bufH));
  h.tap.setDirtyTracking(dirtyTracking);

  RefModel ref(sc.bufW, sc.bufH);

  h.cmd(0x36);  // MADCTL
  h.dat({sc.madctl}, 0, 1);
  ref.madctl = sc.madctl;

  h.cmd(0x3A);  // COLMOD
  h.dat({(uint8_t)(sc.rgb444 ? 0x53 : 0x55)}, 0, 1);
  ref.rgb444 = sc.rgb444;

  if (sc.le) {
    h.cmd(0xB0);  // RAMCTRL: P2 bit3 = ENDIAN
    h.dat({0x00, 0x08}, 0, 1);
    ref.le = true;
  }
  if (sc.invoff) {
    h.cmd(0x20);  // INVOFF (config.inverted=true → output inverted)
    ref.inverter = 0xFFFF;
  }

  h.setWindow(sc.cs, sc.ce, sc.rs, sc.re);
  ref.hwColS = sc.cs;
  ref.hwColE = sc.ce;
  ref.hwRowS = sc.rs;
  ref.hwRowE = sc.re;

  std::vector<uint8_t> payload = randomBytes(sc.numBytes, chunk + stride * 97u);
  h.cmd(0x2C);  // RAMWR
  h.dat(payload, chunk, stride);
  ref.ramwr(payload);

  const uint16_t* fb = h.tap.getFramebuf();
  for (size_t i = 0; i < ref.fb.size(); ++i) {
    if (fb[i] != ref.fb[i]) {
      CHECK(false,
            "%s (chunk=%u stride=%u dirty=%d): fb[%zu] (x=%zu y=%zu) "
            "%04X != ref %04X",
            sc.name, chunk, stride, (int)dirtyTracking, i, i % sc.bufW,
            i / sc.bufW, fb[i], ref.fb[i]);
      return;  // one mismatch per run is enough
    }
  }
}

void runAllDeliveries(const Scenario& sc) {
  printf("  %s\n", sc.name);
  // Chunk sizes crossing pixel/group/row boundaries in different phases;
  // strides: 1 = packed, 4 = PIO ring words, 3 = generic fallback.
  static const uint32_t kChunks[] = {0, 1, 2, 3, 7, 64, 401};
  static const uint32_t kStrides[] = {1, 4, 3};
  for (uint32_t chunk : kChunks) {
    for (uint32_t stride : kStrides) {
      runScenario(sc, chunk, stride, false);
      runScenario(sc, chunk, stride, true);
    }
  }
}

}  // namespace

int main() {
  // clang-format off
  const Scenario scenarios[] = {
    // full-window raster, both formats
    {"rgb565 full", 240, 320, 0x00, false, false, false, 0, 239, 0, 319, 240 * 12 * 2},
    {"rgb444 full", 240, 320, 0x00, true, false, false, 0, 239, 0, 319, 240 * 12 * 3 / 2},
    // little-endian + inversion (RGB565)
    {"rgb565 le+inv", 240, 320, 0x00, false, true, true, 0, 239, 0, 319, 240 * 8 * 2},
    // BGR swap
    {"rgb565 bgr", 240, 320, MADCTL_BGR, false, false, false, 0, 239, 0, 319, 240 * 8 * 2},
    {"rgb444 bgr", 240, 320, MADCTL_BGR, true, false, false, 0, 239, 0, 319, 240 * 8 * 3 / 2},
    // odd window width (RGB444 pending-pixel carry) + odd start (unaligned)
    {"rgb565 odd win", 240, 320, 0x00, false, false, false, 3, 203, 5, 60, 201 * 20 * 2},
    {"rgb444 odd win", 240, 320, 0x00, true, false, false, 3, 203, 5, 60, 201 * 20 * 3 / 2},
    // 1px-wide window (run=1 degenerate)
    {"rgb565 1px win", 240, 320, 0x00, false, false, false, 17, 17, 2, 12, 40 * 2},
    {"rgb444 1px win", 240, 320, 0x00, true, false, false, 17, 17, 2, 12, 30 * 3 / 2 * 2},
    // window wrap-around (payload exceeds the window area)
    {"rgb565 wrap", 96, 64, 0x00, false, false, false, 10, 40, 50, 60, 31 * 11 * 2 * 2 + 34},
    {"rgb444 wrap", 96, 64, 0x00, true, false, false, 10, 40, 50, 60, 31 * 11 * 3 + 35},
    // MADCTL orientations (hStep != +1 scalar paths)
    {"rgb565 mx", 240, 320, MADCTL_MX, false, false, false, 4, 200, 3, 30, 197 * 10 * 2},
    {"rgb565 my", 240, 320, MADCTL_MY, false, false, false, 4, 200, 3, 30, 197 * 10 * 2},
    {"rgb565 mv", 240, 320, MADCTL_MV, false, false, false, 4, 200, 3, 30, 197 * 10 * 2},
    {"rgb565 mv|mx", 240, 320, MADCTL_MV | MADCTL_MX, false, false, false, 4, 200, 3, 30, 197 * 10 * 2},
    {"rgb565 mv|my", 240, 320, MADCTL_MV | MADCTL_MY, false, false, false, 4, 200, 3, 30, 197 * 10 * 2},
    {"rgb565 mx|my", 240, 320, MADCTL_MX | MADCTL_MY, false, false, false, 4, 200, 3, 30, 197 * 10 * 2},
    {"rgb565 mv|mx|my", 240, 320, MADCTL_MV | MADCTL_MX | MADCTL_MY, false, false, false, 4, 200, 3, 30, 197 * 10 * 2},
    {"rgb444 mx", 240, 320, MADCTL_MX, true, false, false, 4, 200, 3, 30, 197 * 10 * 3 / 2},
    {"rgb444 mv", 240, 320, MADCTL_MV, true, false, false, 4, 200, 3, 30, 197 * 10 * 3 / 2},
    {"rgb444 mv|mx|my bgr", 240, 320, MADCTL_MV | MADCTL_MX | MADCTL_MY | MADCTL_BGR, true, false, false, 4, 200, 3, 30, 197 * 10 * 3 / 2},
    // odd-width buffer: row starts alternate 16-bit alignment
    {"rgb565 oddbuf", 61, 37, 0x00, false, false, false, 0, 60, 0, 36, 61 * 37 * 2 + 2},
    {"rgb444 oddbuf", 61, 37, 0x00, true, false, false, 0, 60, 0, 36, 61 * 37 * 3 / 2 + 3},
  };
  // clang-format on

  printf("ramwr_test\n");
  for (const Scenario& sc : scenarios) runAllDeliveries(sc);

  if (gFailures == 0) {
    printf("ALL TESTS PASSED\n");
    return 0;
  }
  printf("%d FAILURE(S)\n", gFailures);
  return 1;
}
