// Host-side unit test for the KS0108 dual-chip graphic LCD controller in
// lib/ (no MCU dependencies).
//
// Feeds KS0108 command streams through the public LcdTap API — including
// the cs chip-select mask — and checks the rendered framebuffer: per-chip
// write separation and duplication, column auto-increment and wrap,
// page/column addressing, display start line scroll (including content
// rotation on change), per-chip display on/off (OR semantics), FlipMode,
// and the cs-ignore contract of single-chip controllers.
//
// Build & run:
//   g++ -O2 -Wall -Wextra -I../../../lib/include -o /tmp/lcdtap_ks0108_test
//       ks0108_test.cpp ../../../lib/src/lcdtap.cpp
//       ../../../lib/src/config.cpp ../../../lib/src/spi_display_base.cpp
//       ../../../lib/src/st7789_controller.cpp
//       ../../../lib/src/ili9341_controller.cpp
//       ../../../lib/src/ssd1306_controller.cpp
//       ../../../lib/src/ssd1331_controller.cpp
//       ../../../lib/src/st7032_controller.cpp
//       ../../../lib/src/ks0108_controller.cpp
//   /tmp/lcdtap_ks0108_test

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lcdtap/devices/ks0108.hpp"
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

constexpr int FB_W = 128;
constexpr int FB_H = 64;

struct Harness {
  LcdTap tap;
  LcdTapConfig cfg;

  explicit Harness(const LcdTapConfig& c)
      : tap(c, HostInterface{testAlloc, testFree, nullptr, nullptr}) {
    cfg = tap.getConfig();
    if (tap.getStatus() != Status::OK) {
      printf("  FATAL: LcdTap init failed\n");
      exit(1);
    }
  }

  void cmd(uint8_t b, uint8_t cs = 3) { tap.inputCommand(b, cs); }
  void data(uint8_t b, uint8_t cs) { tap.inputData(&b, 1, 1, cs); }

  void setPage(uint8_t page, uint8_t cs = 3) {
    cmd(ks0108::CMD_SET_PAGE_BASE | page, cs);
  }
  void setCol(uint8_t col, uint8_t cs = 3) {
    cmd(ks0108::CMD_SET_COL_BASE | col, cs);
  }
  void setStartLine(uint8_t z, uint8_t cs = 3) {
    cmd(ks0108::CMD_SET_START_LINE_BASE | z, cs);
  }
  void displayOn(bool on, uint8_t cs = 3) {
    cmd(ks0108::CMD_DISPLAY_ONOFF_BASE | (on ? 1 : 0), cs);
  }

  bool pixel(int x, int y) const {
    const uint16_t* fb = const_cast<LcdTap&>(tap).getFramebuf();
    return fb[y * FB_W + x] != 0;
  }

  // The 8-bit column pattern currently in the framebuffer at column x,
  // rows [yTop, yTop+7] (bit0 = yTop).
  uint8_t columnBits(int x, int yTop) const {
    uint8_t bits = 0;
    for (int i = 0; i < 8; ++i) {
      if (pixel(x, yTop + i)) bits |= (uint8_t)(1u << i);
    }
    return bits;
  }

  int litCount() const {
    int n = 0;
    for (int y = 0; y < FB_H; ++y) {
      for (int x = 0; x < FB_W; ++x) {
        if (pixel(x, y)) ++n;
      }
    }
    return n;
  }

  bool anyOutputLit() const {
    uint16_t line[640];
    for (uint16_t y = 0; y < 480; ++y) {
      tap.fillScanline(y, line);
      for (int x = 0; x < 640; ++x) {
        if (line[x] != 0) return true;
      }
    }
    return false;
  }
};

LcdTapConfig makeConfig() {
  LcdTapConfig cfg;
  getPresetConfig(ConfigPreset::KS0108, &cfg);
  cfg.dviWidth = 640;
  cfg.dviHeight = 480;
  return cfg;
}

void testChipSelectWrite() {
  printf("chip-select write separation / duplication\n");
  Harness h(makeConfig());
  CHECK(h.cfg.buffWidth == FB_W && h.cfg.buffHeight == FB_H,
        "buffer must be fixed to 128x64 (got %ux%u)", h.cfg.buffWidth,
        h.cfg.buffHeight);

  // cs=1 writes to the left half only
  h.setPage(0, 1);
  h.setCol(5, 1);
  h.data(0x01, 1);  // pixel at (5, 0)
  CHECK(h.pixel(5, 0), "cs=1 write must land in the left half");
  CHECK(!h.pixel(64 + 5, 0), "cs=1 write must not touch the right half");

  // cs=2 writes to the right half only
  h.setPage(0, 2);
  h.setCol(7, 2);
  h.data(0x80, 2);  // pixel at (64+7, 7)
  CHECK(h.pixel(64 + 7, 7), "cs=2 write must land in the right half");
  CHECK(!h.pixel(7, 7), "cs=2 write must not touch the left half");

  // cs=3 duplicates the byte to both chips (at their own addresses)
  h.setPage(1, 3);
  h.setCol(10, 3);
  h.data(0x0F, 3);  // rows 8-11 at column 10 of both chips
  CHECK(h.columnBits(10, 8) == 0x0F, "cs=3 must write the left half");
  CHECK(h.columnBits(64 + 10, 8) == 0x0F, "cs=3 must write the right half");

  // cs=0 must write nothing
  const int lit = h.litCount();
  h.data(0xFF, 0);
  CHECK(h.litCount() == lit, "cs=0 data must be ignored");
}

void testColumnAutoIncrement() {
  printf("column auto-increment and wrap\n");
  Harness h(makeConfig());

  // Column wraps at 64 without crossing into the other chip
  h.setPage(0, 1);
  h.setCol(62, 1);
  for (int i = 0; i < 4; ++i) h.data(0xFF, 1);  // cols 62, 63, 0, 1
  CHECK(h.columnBits(62, 0) == 0xFF, "col 62 must be written");
  CHECK(h.columnBits(63, 0) == 0xFF, "col 63 must be written");
  CHECK(h.columnBits(0, 0) == 0xFF, "col must wrap to 0");
  CHECK(h.columnBits(1, 0) == 0xFF, "col must continue after wrap");
  CHECK(h.columnBits(64, 0) == 0x00, "wrap must not cross into chip 1");

  // Chips keep independent column counters
  h.setCol(0, 1);
  h.setCol(20, 2);
  h.data(0x18, 3);
  CHECK(h.columnBits(0, 0) == 0x18, "chip 0 must write at its own column");
  CHECK(h.columnBits(64 + 20, 0) == 0x18,
        "chip 1 must write at its own column");
}

void testStartLineScroll() {
  printf("display start line scroll\n");
  Harness h(makeConfig());

  // Draw a marker at page 0, then scroll chip 0 by 8 lines
  h.setPage(0, 3);
  h.setCol(5, 3);
  h.data(0x01, 3);  // pixel at (5, 0) on both chips
  h.setStartLine(8, 1);

  // Existing content of chip 0 rotates: row 0 -> row (0-8)&63 = 56
  CHECK(!h.pixel(5, 0), "chip 0 content must scroll away from row 0");
  CHECK(h.pixel(5, 56), "chip 0 content must rotate to row 56");
  // Chip 1 must be unaffected
  CHECK(h.pixel(64 + 5, 0), "chip 1 content must not move");

  // New writes fold the start line into the row mapping:
  // page 1 (memory rows 8-15) now shows at screen rows 0-7
  h.setPage(1, 1);
  h.setCol(6, 1);
  h.data(0x01, 1);
  CHECK(h.pixel(6, 0), "write after scroll must land at the scrolled row");

  // Scrolling back restores the original position
  h.setStartLine(0, 1);
  CHECK(h.pixel(5, 0), "scrolling back must restore content");
  CHECK(h.pixel(6, 8), "content written while scrolled must move back too");
}

void testDisplayOnOff() {
  printf("display on/off (per-chip OR)\n");
  Harness h(makeConfig());

  // RAM writes land regardless of the display state, but the output stays
  // black until a chip is turned on
  h.setPage(0, 3);
  h.setCol(0, 3);
  h.data(0xFF, 3);
  CHECK(h.litCount() != 0, "RAM write must land while display is off");
  CHECK(!h.anyOutputLit(), "output must be black after reset");

  h.displayOn(true, 1);
  CHECK(h.anyOutputLit(), "output must be lit after chip 0 on");

  // Per-chip blanking is not rendered: any chip on keeps the display lit
  h.displayOn(false, 1);
  h.displayOn(true, 2);
  CHECK(h.anyOutputLit(), "output must stay lit while any chip is on");

  h.displayOn(false, 3);
  CHECK(!h.anyOutputLit(), "output must be black when both chips are off");
}

void testFlip() {
  printf("flip modes\n");

  {
    LcdTapConfig cfg = makeConfig();
    cfg.flipMode = FlipMode::FLIP_H;
    Harness h(cfg);
    h.setPage(0, 1);
    h.setCol(0, 1);
    h.data(0x01, 1);  // chip 0, col 0, row 0
    CHECK(h.pixel(FB_W - 1, 0), "H flip must mirror chip 0 to the right edge");
  }

  {
    LcdTapConfig cfg = makeConfig();
    cfg.flipMode = FlipMode::FLIP_V;
    Harness h(cfg);
    h.setPage(0, 1);
    h.setCol(3, 1);
    h.data(0x01, 1);  // row 0 -> physical row 63
    CHECK(h.pixel(3, FB_H - 1), "V flip must mirror row 0 to the bottom");

    // Scroll under V flip: screen row 0 content moves to screen row 56,
    // i.e. physical row 63 -> physical row 63-56 = 7
    h.setStartLine(8, 1);
    CHECK(!h.pixel(3, FB_H - 1), "V-flip scroll must move the content");
    CHECK(h.pixel(3, FB_H - 1 - 56), "V-flip scroll direction must reverse");
  }
}

void testUnknownCommand() {
  printf("unknown command accounting\n");
  Harness h(makeConfig());
  const uint32_t before = h.tap.getUnknownCmdCount();
  h.cmd(0x20, 1);  // not a KS0108 instruction
  CHECK(h.tap.getUnknownCmdCount() == before + 1,
        "0x20 must count as an unknown command");
  CHECK(h.tap.getLastUnknownCmd() == 0x20, "last unknown command must be 0x20");
}

// Single-chip controllers must ignore the cs mask entirely
void testSingleChipIgnoresCs() {
  printf("single-chip controller ignores cs\n");
  LcdTapConfig cfg;
  getPresetConfig(ConfigPreset::ST7789, &cfg);
  cfg.dviWidth = 640;
  cfg.dviHeight = 480;
  Harness h(cfg);

  h.cmd(0x11, 2);  // SLPOUT with cs=2
  h.cmd(0x29, 2);  // DISPON
  h.cmd(0x2C, 2);  // RAMWR
  const uint8_t px[2] = {0xFF, 0xFF};
  h.tap.inputData(px, 2, 1, 2);
  const uint16_t* fb = h.tap.getFramebuf();
  CHECK(fb[0] != 0, "ST7789 must process writes regardless of cs");
}

}  // namespace

int main() {
  testChipSelectWrite();
  testColumnAutoIncrement();
  testStartLineScroll();
  testDisplayOnOff();
  testFlip();
  testUnknownCommand();
  testSingleChipIgnoresCs();

  if (gFailures == 0) {
    printf("all tests passed\n");
    return 0;
  }
  printf("%d failure(s)\n", gFailures);
  return 1;
}
