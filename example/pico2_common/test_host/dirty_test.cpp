// Host-side unit test for the dirty-line tracking in lib/ (no MCU
// dependencies).
//
// Feeds RAMWR command streams through the public LcdTap API and checks that
// the dirty map covers exactly the pixels whose values actually changed, at
// 64px-segment granularity, across interface formats, MADCTL orientations,
// partial windows and chunked delivery. Also checks the presentation epoch
// counter.
//
// Build & run:
//   g++ -O2 -Wall -Wextra -I../../../lib/include -o /tmp/lcdtap_dirty_test
//       dirty_test.cpp ../../../lib/src/lcdtap.cpp
//       ../../../lib/src/config.cpp ../../../lib/src/spi_display_base.cpp
//       ../../../lib/src/st7789_controller.cpp
//       ../../../lib/src/ili9341_controller.cpp
//       ../../../lib/src/ssd1306_controller.cpp
//       ../../../lib/src/ssd1331_controller.cpp
//       ../../../lib/src/st7032_controller.cpp
//       ../../../lib/src/ks0108_controller.cpp
//   /tmp/lcdtap_dirty_test

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

// Test harness: an LcdTap instance plus a framebuffer snapshot used to
// compute the ground-truth dirty map (segment bits of actually-changed
// pixels) for comparison.
struct Harness {
  LcdTap tap;
  uint16_t fbW, fbH;
  std::vector<uint16_t> snapshot;

  explicit Harness(const LcdTapConfig& cfg)
      : tap(cfg, HostInterface{testAlloc, testFree, nullptr, nullptr}) {
    fbW = cfg.buffWidth;
    fbH = cfg.buffHeight;
    if (tap.getStatus() != Status::OK) {
      printf("  FATAL: LcdTap init failed\n");
      exit(1);
    }
    tap.setDirtyTracking(true);
    clearFb();
  }

  void clearFb() {
    memset(tap.getFramebuf(), 0, (size_t)fbW * fbH * sizeof(uint16_t));
    takeSnapshot();
    clearDirty();
  }

  void takeSnapshot() {
    const uint16_t* fb = tap.getFramebuf();
    snapshot.assign(fb, fb + (size_t)fbW * fbH);
  }

  void clearDirty() { memset(tap.dirtyMap(), 0, fbH); }

  void cmd(uint8_t c) { tap.inputCommand(c); }

  void dat(const std::vector<uint8_t>& bytes, uint32_t chunk = 0) {
    if (chunk == 0) chunk = (uint32_t)bytes.size();
    for (size_t i = 0; i < bytes.size(); i += chunk) {
      uint32_t n =
          (uint32_t)((bytes.size() - i < chunk) ? bytes.size() - i : chunk);
      tap.inputData(bytes.data() + i, n);
    }
  }

  void param16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
  }

  void setWindow(uint16_t xs, uint16_t xe, uint16_t ys, uint16_t ye) {
    std::vector<uint8_t> v;
    cmd(0x2A);  // CASET
    param16(v, xs);
    param16(v, xe);
    dat(v);
    v.clear();
    cmd(0x2B);  // RASET
    param16(v, ys);
    param16(v, ye);
    dat(v);
  }

  // Ground truth: segment bits per physical row from the snapshot diff.
  std::vector<uint8_t> expectedDirty() {
    std::vector<uint8_t> exp(fbH, 0);
    const uint16_t* fb = tap.getFramebuf();
    for (uint32_t r = 0; r < fbH; ++r) {
      for (uint32_t c = 0; c < fbW; ++c) {
        if (fb[r * fbW + c] != snapshot[r * fbW + c]) {
          exp[r] |= (uint8_t)(1u << (c >> 6));
        }
      }
    }
    return exp;
  }

  // exact=true: the dirty map must equal the ground truth at segment
  // granularity. exact=false: it must cover the ground truth (over-marking
  // allowed, e.g. under MADCTL MV where a 64px logical run spans rows).
  void checkDirty(bool exact, const char* what) {
    std::vector<uint8_t> exp = expectedDirty();
    const uint8_t* got = tap.dirtyMap();
    for (uint32_t r = 0; r < fbH; ++r) {
      CHECK((got[r] & exp[r]) == exp[r],
            "%s: row %u: dirty %02X does not cover expected %02X", what, r,
            got[r], exp[r]);
      if (exact) {
        CHECK(got[r] == exp[r], "%s: row %u: dirty %02X != expected %02X", what,
              r, got[r], exp[r]);
      }
    }
  }
};

LcdTapConfig st7789Config(uint16_t w, uint16_t h) {
  LcdTapConfig cfg;
  getDefaultConfig(ControllerFamily::ST7789, &cfg);
  cfg.buffWidth = w;
  cfg.buffHeight = h;
  cfg.dviWidth = 640;
  cfg.dviHeight = 480;
  return cfg;
}

std::vector<uint8_t> pixels565(const std::vector<uint16_t>& px) {
  std::vector<uint8_t> v;
  for (uint16_t p : px) {
    v.push_back((uint8_t)(p >> 8));
    v.push_back((uint8_t)p);
  }
  return v;
}

//---------------------------------------------------------------------------

void testBasicChangeDetection() {
  printf("testBasicChangeDetection\n");
  Harness h(st7789Config(240, 320));

  // Writing zeros over a zeroed framebuffer must produce no dirty bits.
  h.setWindow(0, 239, 0, 319);
  h.cmd(0x2C);
  h.dat(std::vector<uint8_t>(240 * 4 * 2, 0x00));
  h.checkDirty(true, "same-value write");

  // Change two pixels: (5, 2) and (200, 7) -> rows 2 and 7, segments 0 and 3.
  h.cmd(0x00);  // NOP: leave RAMWR so the next RAMWR restarts at the window
  h.setWindow(5, 5, 2, 2);
  h.cmd(0x2C);
  h.dat(pixels565({0x1234}));
  h.setWindow(200, 200, 7, 7);
  h.cmd(0x2C);
  h.dat(pixels565({0xABCD}));
  h.checkDirty(true, "two-pixel write");

  // Rewriting the identical full screen content must add nothing.
  h.takeSnapshot();
  h.clearDirty();
  std::vector<uint16_t> full(240, 0);
  h.setWindow(0, 239, 0, 319);
  h.cmd(0x2C);
  for (uint32_t y = 0; y < 320; ++y) {
    std::vector<uint16_t> row(240, 0);
    if (y == 2) row[5] = 0x1234;
    if (y == 7) row[200] = 0xABCD;
    h.dat(pixels565(row));
  }
  h.checkDirty(true, "identical full-screen rewrite");
}

void testChunkedDelivery() {
  printf("testChunkedDelivery\n");
  Harness h(st7789Config(240, 64));

  // Same pixel stream, delivered in 3-byte chunks (odd size relative to the
  // 2-byte RGB565 pixels, so chunk ends fall inside pixels and rows).
  std::vector<uint16_t> px;
  for (uint32_t i = 0; i < 240u * 64u; ++i)
    px.push_back((uint16_t)(i * 2654435761u >> 16));
  h.setWindow(0, 239, 0, 63);
  h.cmd(0x2C);
  h.dat(pixels565(px), 3);
  h.checkDirty(true, "chunked full write");
}

void testWindowAndWrap() {
  printf("testWindowAndWrap\n");
  Harness h(st7789Config(240, 320));

  // 10x3 window write with wrap-around: 4 rows of data into a 3-row window.
  h.setWindow(60, 69, 100, 102);
  h.cmd(0x2C);
  std::vector<uint16_t> px(10 * 4, 0x5555);
  h.dat(pixels565(px));
  h.checkDirty(true, "window wrap write");
}

void testMadctlOrientations() {
  printf("testMadctlOrientations\n");
  for (uint8_t madctl : {0x00u, 0x20u, 0x40u, 0x80u, 0x60u, 0xC0u, 0xE0u}) {
    Harness h(st7789Config(240, 320));
    h.cmd(0x36);  // MADCTL
    h.dat({madctl});

    // Logical dimensions swap when MV (0x20) is set.
    uint16_t lw = (madctl & 0x20u) ? 320 : 240;
    (void)lw;
    h.takeSnapshot();
    h.clearDirty();

    // Write a 40x5 block at logical (17, 3) with a value pattern.
    h.setWindow(17, 56, 3, 7);
    h.cmd(0x2C);
    std::vector<uint16_t> px;
    for (uint32_t i = 0; i < 40u * 5u; ++i) px.push_back((uint16_t)(i + 1));
    h.dat(pixels565(px), 7);

    // MV maps a 64px logical run onto up to 64 physical rows, so only
    // coverage (not exactness) is required there.
    char what[64];
    snprintf(what, sizeof(what), "MADCTL %02X", madctl);
    h.checkDirty((madctl & 0x20u) == 0, what);
  }
}

void testInterfaceFormats() {
  printf("testInterfaceFormats\n");
  // RGB444 (3 bytes = 2 px) and RGB666 (3 bytes = 1 px) exercise the
  // drain/tight-loop/remainder paths.
  for (int fmt : {0x03, 0x06}) {
    LcdTapConfig cfg = st7789Config(240, 64);
    cfg.interfaceFormatOverride = (int8_t)fmt;
    Harness h(cfg);
    h.setWindow(0, 239, 0, 63);
    h.cmd(0x2C);
    std::vector<uint8_t> bytes(240 * 64 * 3 / ((fmt == 0x03) ? 2 : 1));
    for (size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = (uint8_t)(i * 37u + 11u);
    h.dat(bytes, 5);
    char what[32];
    snprintf(what, sizeof(what), "format %02X", fmt);
    h.checkDirty(true, what);

    // Identical rewrite: no new dirt.
    h.takeSnapshot();
    h.clearDirty();
    h.cmd(0x00);
    h.setWindow(0, 239, 0, 63);
    h.cmd(0x2C);
    h.dat(bytes, 5);
    h.checkDirty(true, what);
  }
}

void testSsd1306() {
  printf("testSsd1306\n");
  LcdTapConfig cfg;
  getDefaultConfig(ControllerFamily::SSD1306, &cfg);
  cfg.dviWidth = 640;
  cfg.dviHeight = 480;
  Harness h(cfg);

  // Page-mode write at page 0, column 0: 0x00 over zeroed fb -> clean,
  // then 0x0F -> rows 0..3 of column 0 change.
  h.tap.inputData((const uint8_t[]){0x00}, 1);
  h.checkDirty(true, "ssd1306 same-value byte");
  h.tap.inputData((const uint8_t[]){0x0F}, 1);
  h.checkDirty(false, "ssd1306 changed byte");
}

// Deterministic RNG for the streaming tests.
struct Lcg {
  uint32_t s;
  uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
};

// Emulate the DisplayLink pump's claim: read-and-clear each dirty row and
// copy the covered framebuffer segments into a model of the device
// framebuffer (in source coordinates; the output scaling math is exercised
// elsewhere). Interleaved with input processing this checks the central
// eventual-consistency property: after a settle claim, the device model must
// equal the framebuffer exactly, i.e. no change may ever escape the map.
void claimToDevice(Harness& h, std::vector<uint16_t>& device) {
  uint8_t* map = h.tap.dirtyMap();
  const uint16_t* fb = h.tap.getFramebuf();
  for (uint32_t r = 0; r < h.fbH; ++r) {
    uint8_t bits = map[r];
    if (bits == 0) continue;
    map[r] = 0;
    for (uint32_t k = 0; k < 8u; ++k) {
      if (!(bits & (1u << k))) continue;
      uint32_t c0 = k << 6;
      if (c0 >= h.fbW) continue;
      uint32_t c1 = c0 + 63u;
      if (c1 > (uint32_t)h.fbW - 1u) c1 = h.fbW - 1u;
      for (uint32_t c = c0; c <= c1; ++c) {
        device[r * h.fbW + c] = fb[r * h.fbW + c];
      }
    }
  }
}

void checkDeviceMatchesFb(Harness& h, const std::vector<uint16_t>& device,
                          const char* what) {
  const uint16_t* fb = h.tap.getFramebuf();
  int bad = 0;
  uint32_t firstAt = 0;
  for (uint32_t i = 0; i < (uint32_t)h.fbW * h.fbH; ++i) {
    if (device[i] != fb[i]) {
      if (bad == 0) firstAt = i;
      ++bad;
    }
  }
  CHECK(bad == 0, "%s: %d stale pixel(s), first at (%u, %u)", what, bad,
        firstAt % h.fbW, firstAt / h.fbW);
}

// Arduboy-style workload: SSD1306 in horizontal addressing mode streaming
// full 1KB frames back to back, with random per-frame mutations, random
// chunk boundaries and pump claims interleaved mid-frame.
void testSsd1306Streaming() {
  printf("testSsd1306Streaming\n");
  for (int remap = 0; remap < 2; ++remap) {
    for (int comdec = 0; comdec < 2; ++comdec) {
      LcdTapConfig cfg;
      getDefaultConfig(ControllerFamily::SSD1306, &cfg);
      cfg.dviWidth = 640;
      cfg.dviHeight = 480;
      Harness h(cfg);

      h.cmd(0x20);
      h.cmd(0x00);  // horizontal addressing mode
      h.cmd(remap ? 0xA1 : 0xA0);
      h.cmd(comdec ? 0xC8 : 0xC0);
      h.cmd(0x21);  // column address window
      h.cmd(0);
      h.cmd((uint8_t)(h.fbW - 1));
      h.cmd(0x22);  // page address window
      h.cmd(0);
      h.cmd((uint8_t)(h.fbH / 8 - 1));
      h.clearFb();

      const uint32_t frameBytes = (uint32_t)h.fbW * h.fbH / 8u;
      std::vector<uint16_t> device((size_t)h.fbW * h.fbH, 0);
      std::vector<uint8_t> pattern(frameBytes, 0);
      Lcg rng{0x1234u + (uint32_t)(remap * 7 + comdec * 13)};

      for (int frame = 0; frame < 100; ++frame) {
        uint32_t muts = 1u + rng.next() % 30u;
        for (uint32_t m = 0; m < muts; ++m) {
          pattern[rng.next() % frameBytes] = (uint8_t)rng.next();
        }
        uint32_t pos = 0;
        while (pos < frameBytes) {
          uint32_t n = 1u + rng.next() % 97u;
          if (pos + n > frameBytes) n = frameBytes - pos;
          h.tap.inputData(pattern.data() + pos, n);
          pos += n;
          if (rng.next() % 3u == 0u) claimToDevice(h, device);
        }
      }
      claimToDevice(h, device);
      char what[64];
      snprintf(what, sizeof(what), "ssd1306 stream remap=%d comdec=%d", remap,
               comdec);
      checkDeviceMatchesFb(h, device, what);
    }
  }
}

// ST7789 equivalent: continuous full-window RGB565 frames with random
// mutations, random chunk boundaries and mid-frame claims, across MADCTL
// orientations.
void testSt7789Streaming() {
  printf("testSt7789Streaming\n");
  for (uint8_t madctl : {0x00u, 0x20u, 0xC0u, 0xE0u}) {
    Harness h(st7789Config(240, 320));
    h.cmd(0x36);
    h.dat({madctl});
    h.clearFb();

    uint16_t lw = (madctl & 0x20u) ? 320 : 240;
    uint16_t lh = (madctl & 0x20u) ? 240 : 320;
    const uint32_t framePx = (uint32_t)lw * lh;
    std::vector<uint16_t> device((size_t)h.fbW * h.fbH, 0);
    std::vector<uint16_t> pattern(framePx, 0);
    Lcg rng{0xBEEFu + madctl};

    h.setWindow(0, (uint16_t)(lw - 1), 0, (uint16_t)(lh - 1));
    h.cmd(0x2C);  // stream continuously with wrap-around
    for (int frame = 0; frame < 30; ++frame) {
      uint32_t muts = 1u + rng.next() % 200u;
      for (uint32_t m = 0; m < muts; ++m) {
        pattern[rng.next() % framePx] = (uint16_t)rng.next();
      }
      std::vector<uint8_t> bytes = pixels565(pattern);
      uint32_t pos = 0;
      while (pos < bytes.size()) {
        uint32_t n = 1u + rng.next() % 331u;
        if (pos + n > bytes.size()) n = (uint32_t)bytes.size() - pos;
        h.tap.inputData(bytes.data() + pos, n);
        pos += n;
        if (rng.next() % 3u == 0u) claimToDevice(h, device);
      }
    }
    claimToDevice(h, device);
    char what[48];
    snprintf(what, sizeof(what), "st7789 stream MADCTL %02X", madctl);
    checkDeviceMatchesFb(h, device, what);
  }
}

void testEpoch() {
  printf("testEpoch\n");
  Harness h(st7789Config(240, 320));
  uint32_t e0 = h.tap.getPresentationEpoch();

  // Plain pixel writes must not bump the epoch.
  h.setWindow(0, 9, 0, 0);
  h.cmd(0x2C);
  h.dat(pixels565(std::vector<uint16_t>(10, 0x1111)));
  CHECK(h.tap.getPresentationEpoch() == e0, "epoch changed by RAMWR");

  h.cmd(0x21);  // INVON
  CHECK(h.tap.getPresentationEpoch() != e0, "INVON did not bump epoch");

  uint32_t e1 = h.tap.getPresentationEpoch();
  h.cmd(0x11);  // SLPOUT
  h.cmd(0x29);  // DISPON
  CHECK(h.tap.getPresentationEpoch() != e1, "SLPOUT/DISPON did not bump");

  uint32_t e2 = h.tap.getPresentationEpoch();
  h.tap.setOutputRotation(1);
  CHECK(h.tap.getPresentationEpoch() != e2, "rotation did not bump epoch");
}

void testOutputMapInfo() {
  printf("testOutputMapInfo\n");
  Harness h(st7789Config(240, 320));
  OutputMapInfo mi;
  h.tap.getOutputMapInfo(&mi);
  CHECK(mi.blanked, "expected blanked before SLPOUT/DISPON");
  h.cmd(0x11);
  h.cmd(0x29);
  h.tap.getOutputMapInfo(&mi);
  CHECK(!mi.blanked, "expected unblanked after SLPOUT+DISPON");
  CHECK(mi.fbWidth == 240 && mi.fbHeight == 320, "fb size mismatch");
  CHECK(mi.destW > 0 && mi.destH > 0, "empty dest rect");
  CHECK(mi.stepV != 0 && mi.stepH != 0, "zero step");

  // Cross-check the documented mapping against fillScanline: the first and
  // last active output lines must read from the first and last source rows.
  std::vector<uint16_t> line(640);
  uint16_t* fb = h.tap.getFramebuf();
  for (uint32_t c = 0; c < 240; ++c) fb[c] = 0x1111;                // row 0
  for (uint32_t c = 0; c < 240; ++c) fb[319u * 240u + c] = 0x2222;  // row 319
  h.tap.fillScanline(mi.destY, line.data());
  CHECK(line[mi.destX + mi.destW / 2] == 0x1111, "first line mapping");
  h.tap.fillScanline((uint16_t)(mi.destY + mi.destH - 1), line.data());
  CHECK(line[mi.destX + mi.destW / 2] == 0x2222, "last line mapping");
}

void testTrackingGate() {
  printf("testTrackingGate\n");
  // Framebuffers beyond MAX_DIRTY_ROWS in either dimension refuse tracking.
  LcdTapConfig cfg = st7789Config(240, 320);
  {
    Harness h(cfg);
    CHECK(h.tap.isDirtyTrackingActive(), "tracking should be active");
    h.tap.setDirtyTracking(false);
    CHECK(!h.tap.isDirtyTrackingActive(), "tracking should be off");
  }
}

}  // namespace

int main() {
  testBasicChangeDetection();
  testChunkedDelivery();
  testWindowAndWrap();
  testMadctlOrientations();
  testInterfaceFormats();
  testSsd1306();
  testSsd1306Streaming();
  testSt7789Streaming();
  testEpoch();
  testOutputMapInfo();
  testTrackingGate();

  if (gFailures == 0) {
    printf("ALL TESTS PASSED\n");
    return 0;
  }
  printf("%d FAILURE(S)\n", gFailures);
  return 1;
}
