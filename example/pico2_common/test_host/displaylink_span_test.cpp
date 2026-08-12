// Host-side test for the DisplayLink pump's output-mapping helpers
// (displaylink_map.hpp) against the real LcdTap::fillScanline.
//
// For every source column (or row, for the rotated orientations), lights it
// up with a unique value, fills a scanline, finds the lit output x range
// (ground truth), and checks that srcRangeToSpan covers it. Also checks the
// vertical duplicate-group formula. Catches fixed-point drift between
// fillScanline's sampling and the pump's span math — the bug behind the
// stale vertical lines at 64px segment boundaries under rot=180.
//
// Build & run:
//   g++ -O2 -Wall -Wextra -I../../../lib/include
//   -I../../pico2_universal/include
//       -o /tmp/lcdtap_span_test displaylink_span_test.cpp
//       ../../../lib/src/lcdtap.cpp ../../../lib/src/config.cpp
//       ../../../lib/src/spi_display_base.cpp
//       ../../../lib/src/st7789_controller.cpp
//       ../../../lib/src/ili9341_controller.cpp
//       ../../../lib/src/ssd1306_controller.cpp
//       ../../../lib/src/ssd1331_controller.cpp
//       ../../../lib/src/st7032_controller.cpp
//       ../../../lib/src/ks0108_controller.cpp
//   /tmp/lcdtap_span_test

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "displaylink_map.hpp"
#include "lcdtap/lcdtap.hpp"

using namespace lcdtap;

static int gFailures = 0;
#define CHECK(cond, ...)               \
  do {                                 \
    if (!(cond)) {                     \
      printf("  FAIL %d: ", __LINE__); \
      printf(__VA_ARGS__);             \
      printf("\n");                    \
      ++gFailures;                     \
    }                                  \
  } while (0)

static void* testAlloc(size_t size) { return malloc(size); }
static void testFree(void* ptr) { free(ptr); }

static void testConfig(const char* name, ControllerFamily fam, uint16_t fbW,
                       uint16_t fbH, uint16_t dviW, uint16_t dviH,
                       ScaleMode scale, uint8_t rot) {
  LcdTapConfig cfg;
  getDefaultConfig(fam, &cfg);
  cfg.buffWidth = fbW;
  cfg.buffHeight = fbH;
  cfg.dviWidth = dviW;
  cfg.dviHeight = dviH;
  cfg.scaleMode = scale;
  cfg.outputRotation = rot;
  LcdTap tap(cfg, HostInterface{testAlloc, testFree, nullptr, nullptr});
  if (tap.getStatus() != Status::OK) {
    printf("  init failed for %s\n", name);
    ++gFailures;
    return;
  }
  tap.setDisplayOn(true);

  OutputMapInfo mi;
  tap.getOutputMapInfo(&mi);
  if (mi.blanked || mi.destW == 0 || mi.destH == 0) {
    printf("  %s: blanked/empty dest\n", name);
    ++gFailures;
    return;
  }

  uint16_t* fb = tap.getFramebuf();
  std::vector<uint16_t> line(dviW);
  const bool rotOdd = (rot & 1u) != 0;
  const bool mirrored = (rot == 2) || (rot == 1);
  int failsBefore = gFailures;

  // Along-line coverage: source index -> output x span.
  const uint32_t nIdx = rotOdd ? mi.srcH : mi.srcW;
  for (uint32_t c = 0; c < nIdx; ++c) {
    // Light one full source column (rot even) or row (rot odd).
    memset(fb, 0, (size_t)fbW * fbH * sizeof(uint16_t));
    if (!rotOdd) {
      uint32_t col = mi.srcX + c;
      for (uint32_t r = 0; r < fbH; ++r) fb[r * fbW + col] = 0xFFFFu;
    } else {
      uint32_t row = mi.srcY + c;
      for (uint32_t x = 0; x < fbW; ++x) fb[row * fbW + x] = 0xFFFFu;
    }

    // Ground truth from the real fillScanline on a middle active line.
    uint32_t y = mi.destY + mi.destH / 2;
    tap.fillScanline((uint16_t)y, line.data());
    int32_t lit0 = -1, lit1 = -1;
    for (uint32_t x = 0; x < dviW; ++x) {
      if (line[x] == 0xFFFFu) {
        if (lit0 < 0) lit0 = (int32_t)x;
        lit1 = (int32_t)x;
      }
    }
    if (lit0 < 0) continue;  // index skipped by downscale sampling

    uint32_t x0, x1;
    bool ok = srcRangeToSpan(mi, c, c, mirrored, &x0, &x1);
    CHECK(ok, "%s: idx %u: no span but lit %d..%d", name, c, lit0, lit1);
    if (!ok) continue;
    CHECK((int32_t)x0 <= lit0 && (int32_t)x1 >= lit1,
          "%s: idx %u: span [%u..%u] does not cover lit [%d..%d]", name, c, x0,
          x1, lit0, lit1);
  }

  // Vertical: every active output line belongs to its own source line group.
  for (uint32_t y = mi.destY; y < (uint32_t)(mi.destY + mi.destH); ++y) {
    uint32_t t = (uint32_t)(((uint64_t)(y - mi.destY) * mi.stepV) >> 16);
    uint32_t g0 = groupFirstLine(mi, t);
    uint32_t g1 = groupFirstLine(mi, t + 1u);
    g1 = (g1 > g0) ? g1 - 1u : g0;
    CHECK(y >= g0 && y <= g1, "%s: line %u outside its group [%u..%u] t=%u",
          name, y, g0, g1, t);
  }
  if (gFailures == failsBefore) printf("  %s: OK\n", name);
}

int main() {
  for (uint8_t rot = 0; rot < 4; ++rot) {
    char name[64];
    snprintf(name, sizeof(name), "arduboy FIT rot%u", rot);
    testConfig(name, ControllerFamily::SSD1306, 128, 64, 1280, 720,
               ScaleMode::FIT, rot);
    snprintf(name, sizeof(name), "arduboy INTEGRAL rot%u", rot);
    testConfig(name, ControllerFamily::SSD1306, 128, 64, 1280, 720,
               ScaleMode::INTEGRAL, rot);
    snprintf(name, sizeof(name), "arduboy 1080p rot%u", rot);
    testConfig(name, ControllerFamily::SSD1306, 128, 64, 1920, 1080,
               ScaleMode::FIT, rot);
    snprintf(name, sizeof(name), "st7789 240x240 rot%u", rot);
    testConfig(name, ControllerFamily::ST7789, 240, 240, 1280, 720,
               ScaleMode::FIT, rot);
    snprintf(name, sizeof(name), "st7789 240x320 rot%u", rot);
    testConfig(name, ControllerFamily::ST7789, 240, 320, 1280, 720,
               ScaleMode::FIT, rot);
    snprintf(name, sizeof(name), "st7789 320x480 rot%u", rot);
    testConfig(name, ControllerFamily::ST7789, 320, 480, 1920, 1080,
               ScaleMode::STRETCH, rot);
  }

  if (gFailures == 0) {
    printf("ALL SPAN COVERAGE TESTS PASSED\n");
    return 0;
  }
  printf("%d FAILURE(S)\n", gFailures);
  return 1;
}
