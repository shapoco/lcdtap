// Host-side unit test for the ST7032 character LCD controller in lib/ (no
// MCU dependencies).
//
// Feeds HD44780/ST7032 command streams through the public LcdTap API and
// checks the rendered framebuffer: DDRAM addressing and wrap, display shift,
// entry mode, CGRAM/OPR replacement, cursor (OR-lit underline), blink via
// tick(), double height, 4-bit nibble pairing and the 4-line fold layout.
//
// Build & run:
//   g++ -O2 -Wall -Wextra -I../../../lib/include -o /tmp/lcdtap_st7032_test
//       st7032_test.cpp ../../../lib/src/lcdtap.cpp
//       ../../../lib/src/config.cpp ../../../lib/src/spi_display_base.cpp
//       ../../../lib/src/st7789_controller.cpp
//       ../../../lib/src/ili9341_controller.cpp
//       ../../../lib/src/ssd1306_controller.cpp
//       ../../../lib/src/ssd1331_controller.cpp
//       ../../../lib/src/st7032_controller.cpp
//   /tmp/lcdtap_st7032_test

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lcdtap/font_st7032.hpp"
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

constexpr uint16_t CELL_W = 6;
constexpr uint16_t CELL_H = 9;

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

  void cmd(uint8_t b) { tap.inputCommand(b); }
  void data(uint8_t b) { tap.inputData(&b, 1); }

  bool pixel(int col, int row, int gx, int gy) const {
    const uint16_t* fb = const_cast<LcdTap&>(tap).getFramebuf();
    int x = col * CELL_W + gx;
    int y = row * CELL_H + gy;
    return fb[y * cfg.buffWidth + x] != 0;
  }

  // Compare the 5x8 pixels of a cell against a glyph (5-bit row patterns,
  // bit4 = leftmost pixel). rowOverride >= 0 forces that row all-on
  // (cursor underline).
  bool cellMatches(int col, int row, const uint8_t* glyph,
                   int rowOverride = -1) const {
    for (int gy = 0; gy < 8; ++gy) {
      uint8_t bits = glyph[gy] & 0x1F;
      if (gy == rowOverride) bits = 0x1F;
      for (int gx = 0; gx < 5; ++gx) {
        bool expect = (bits >> (4 - gx)) & 1;
        if (pixel(col, row, gx, gy) != expect) return false;
      }
    }
    return true;
  }

  bool cellShowsCode(int col, int row, uint8_t code) const {
    return cellMatches(col, row, &font_st7032::bitmap[(uint32_t)code * 8]);
  }

  bool cellBlank(int col, int row) const {
    for (int gy = 0; gy < 8; ++gy) {
      for (int gx = 0; gx < 5; ++gx) {
        if (pixel(col, row, gx, gy)) return false;
      }
    }
    return true;
  }

  bool cellAllOn(int col, int row) const {
    for (int gy = 0; gy < 8; ++gy) {
      for (int gx = 0; gx < 5; ++gx) {
        if (!pixel(col, row, gx, gy)) return false;
      }
    }
    return true;
  }
};

LcdTapConfig makeConfig(ConfigPreset preset) {
  LcdTapConfig cfg;
  getPresetConfig(preset, &cfg);
  cfg.dviWidth = 640;
  cfg.dviHeight = 480;
  return cfg;
}

// Standard 8-bit init: 2-line mode, display on, clear
void init8bit(Harness& h) {
  h.cmd(0x38);  // Function Set: DL=1, N=1
  h.cmd(0x0C);  // Display ON, cursor off
  h.cmd(0x01);  // Clear
  h.cmd(0x06);  // Entry: increment, no shift
}

void testBasicWrite() {
  printf("basic write / addressing\n");
  Harness h(makeConfig(ConfigPreset::TEXT_1602));
  CHECK(h.cfg.buffWidth == 95 && h.cfg.buffHeight == 17,
        "fb size %dx%d != 95x17", h.cfg.buffWidth, h.cfg.buffHeight);
  init8bit(h);

  h.cmd(0x80);  // DDRAM addr 0
  h.data('H');
  h.data('i');
  CHECK(h.cellShowsCode(0, 0, 'H'), "cell(0,0) != 'H'");
  CHECK(h.cellShowsCode(1, 0, 'i'), "cell(1,0) != 'i'");
  CHECK(h.cellBlank(2, 0), "cell(2,0) not blank");

  // Second line
  h.cmd(0x80 | 0x40);
  h.data('2');
  CHECK(h.cellShowsCode(0, 1, '2'), "cell(0,1) != '2'");

  // AC wrap: 0x27 -> 0x40 on increment
  h.cmd(0x80 | 0x27);
  h.data('A');  // invisible (col 39 of line 1)
  h.data('B');  // lands at 0x40 -> cell(0,1)
  CHECK(h.cellShowsCode(0, 1, 'B'), "AC wrap 0x27->0x40 failed");

  // Decrement entry mode
  h.cmd(0x04);  // I/D=0
  h.cmd(0x80 | 0x02);
  h.data('C');  // addr 2, then AC=1
  h.data('D');  // addr 1
  CHECK(h.cellShowsCode(2, 0, 'C'), "cell(2,0) != 'C'");
  CHECK(h.cellShowsCode(1, 0, 'D'), "cell(1,0) != 'D' (decrement)");

  // Clear display blanks everything
  h.cmd(0x01);
  CHECK(h.cellBlank(0, 0) && h.cellBlank(0, 1), "clear left content");
}

void testShift() {
  printf("display shift / return home / entry shift\n");
  Harness h(makeConfig(ConfigPreset::TEXT_1602));
  init8bit(h);
  h.cmd(0x80);
  h.data('A');
  h.data('B');

  // Shift left: position 0 now shows DDRAM addr 1 ('B')
  h.cmd(0x18);  // SC=1, RL=0
  CHECK(h.cellShowsCode(0, 0, 'B'), "shift left: cell(0,0) != 'B'");

  // Shift right twice: position 1 shows addr 0 ('A')
  h.cmd(0x1C);  // SC=1, RL=1
  h.cmd(0x1C);
  CHECK(h.cellShowsCode(1, 0, 'A'), "shift right: cell(1,0) != 'A'");

  // Return home resets the shift
  h.cmd(0x02);
  CHECK(h.cellShowsCode(0, 0, 'A'), "return home: cell(0,0) != 'A'");

  // Entry mode S=1: display shifts along with each write
  h.cmd(0x01);
  h.cmd(0x07);  // I/D=1, S=1
  h.cmd(0x80);
  h.data('X');  // ddram[0]='X', shift -> 1: 'X' scrolls out to the left
  CHECK(h.cellBlank(0, 0), "S=1: cell(0,0) should be blank after scroll");
}

void testCgramOpr() {
  printf("CGRAM / OPR replacement\n");
  // TEXT_1602 preset: opr=1 -> codes 0x00-0x07 come from CGRAM
  Harness h(makeConfig(ConfigPreset::TEXT_1602));
  init8bit(h);

  // Define CGRAM char 0: horizontal stripes
  static const uint8_t kStripes[8] = {0x1F, 0x00, 0x1F, 0x00,
                                      0x1F, 0x00, 0x1F, 0x00};
  h.cmd(0x40);  // CGRAM addr 0
  for (int i = 0; i < 8; ++i) h.data(kStripes[i]);

  h.cmd(0x80);
  h.data(0x00);  // CGRAM char 0
  h.data(0x08);  // opr=1: NOT replaced; comes from CGROM
  CHECK(h.cellMatches(0, 0, kStripes), "CGRAM char 0 not rendered");
  CHECK(h.cellShowsCode(1, 0, 0x08), "code 0x08 should use CGROM at opr=1");

  // Redefining the CGRAM pattern re-renders cells that use it
  h.cmd(0x40);
  static const uint8_t kBox[8] = {0x1F, 0x11, 0x11, 0x11,
                                  0x11, 0x11, 0x11, 0x1F};
  for (int i = 0; i < 8; ++i) h.data(kBox[i]);
  CHECK(h.cellMatches(0, 0, kBox), "CGRAM redefine not re-rendered");
}

void testCursor() {
  printf("cursor underline / blink\n");
  Harness h(makeConfig(ConfigPreset::TEXT_1602));
  init8bit(h);
  h.cmd(0x80);
  h.data('A');  // AC now 1 -> cursor cell (1,0)

  // C=1: the 8th glyph line of the cursor cell is OR-lit
  h.cmd(0x0E);  // D=1, C=1, B=0
  CHECK(h.cellMatches(1, 0, &font_st7032::bitmap[' ' * 8], /*rowOverride=*/7),
        "cursor underline not lit");
  CHECK(h.cellShowsCode(0, 0, 'A'), "cursor leaked into cell(0,0)");

  // Cursor follows a DDRAM address change
  h.cmd(0x80 | 0x05);
  CHECK(h.cellShowsCode(1, 0, ' '), "old cursor cell not restored");
  CHECK(h.cellMatches(5, 0, &font_st7032::bitmap[' ' * 8], 7),
        "cursor did not move to cell(5,0)");

  // B=1: tick() alternates the whole cell between all-on and normal
  h.cmd(0x0D);  // D=1, C=0, B=1
  h.tap.tick(600);
  CHECK(h.cellAllOn(5, 0), "blink phase 1: cell not all-on");
  h.tap.tick(1200);
  CHECK(h.cellShowsCode(5, 0, ' '), "blink phase 2: cell not restored");

  // Cursor hidden entirely when C=B=0
  h.cmd(0x0C);
  CHECK(h.cellShowsCode(5, 0, ' '), "cursor not hidden");
}

void testDoubleHeight() {
  printf("double height (N=0, DH=1)\n");
  Harness h(makeConfig(ConfigPreset::TEXT_1602));
  h.cmd(0x34);  // Function Set: DL=1, N=0, DH=1
  h.cmd(0x0C);
  h.cmd(0x01);
  h.cmd(0x80);
  h.data('X');

  const uint8_t* glyph = &font_st7032::bitmap['X' * 8];
  const uint16_t* fb = h.tap.getFramebuf();
  bool ok = true;
  for (int gy = 0; gy < 16 && ok; ++gy) {
    uint8_t bits = glyph[gy / 2] & 0x1F;
    for (int gx = 0; gx < 5; ++gx) {
      bool expect = (bits >> (4 - gx)) & 1;
      if ((fb[gy * h.cfg.buffWidth + gx] != 0) != expect) ok = false;
    }
  }
  CHECK(ok, "double-height glyph mismatch");
}

void testOneLineMode() {
  printf("1-line mode addressing (N=0)\n");
  Harness h(makeConfig(ConfigPreset::TEXT_1602));
  h.cmd(0x30);  // DL=1, N=0
  h.cmd(0x0C);
  h.cmd(0x01);
  h.cmd(0x06);

  // Second line is hidden in 1-line mode
  h.cmd(0x80);
  h.data('A');
  CHECK(h.cellShowsCode(0, 0, 'A'), "cell(0,0) != 'A' in 1-line mode");

  // AC wraps 0x4F -> 0x00 (contiguous 80-char line)
  h.cmd(0x80 | 0x4F);
  h.data('B');  // invisible (position 79)
  h.data('C');  // wraps to addr 0
  CHECK(h.cellShowsCode(0, 0, 'C'), "AC wrap 0x4F->0x00 failed");
}

void testFourRowFold() {
  printf("4-row fold layout (rows=4)\n");
  LcdTapConfig cfg = makeConfig(ConfigPreset::TEXT_2004);
  cfg.busInterface = BusType::I2C;  // exercise plain 8-bit transfers
  Harness h(cfg);
  CHECK(h.cfg.buffWidth == 119 && h.cfg.buffHeight == 35,
        "fb size %dx%d != 119x35", h.cfg.buffWidth, h.cfg.buffHeight);
  init8bit(h);

  h.cmd(0x80 | 0x00);
  h.data('1');  // row 0
  h.cmd(0x80 | 0x40);
  h.data('2');  // row 1
  h.cmd(0x80 | 0x14);
  h.data('3');  // cols offset on line 1 -> visible row 2
  h.cmd(0x80 | 0x54);
  h.data('4');  // cols offset on line 2 -> visible row 3
  CHECK(h.cellShowsCode(0, 0, '1'), "cell(0,0) != '1'");
  CHECK(h.cellShowsCode(0, 1, '2'), "cell(0,1) != '2'");
  CHECK(h.cellShowsCode(0, 2, '3'), "cell(0,2) != '3'");
  CHECK(h.cellShowsCode(0, 3, '4'), "cell(0,3) != '4'");
}

void testNibbleMode() {
  printf("4-bit nibble pairing (parallel bus)\n");
  // TEXT_1604 preset: PARALLEL bus, so DL=0 activates nibble mode
  Harness h(makeConfig(ConfigPreset::TEXT_1604));

  // Standard 4-bit init as seen through the upper-nibble wiring:
  // lone nibbles 0x3,0x3,0x3 then 0x2 arrive as full bytes with the low
  // nibble pulled to zero.
  h.cmd(0x30);
  h.cmd(0x30);
  h.cmd(0x30);
  h.cmd(0x20);  // Function Set DL=0 -> nibble mode from here on
  // Function Set 0x28 (N=1) as a high/low nibble pair
  h.cmd(0x20);
  h.cmd(0x80);
  // Display ON 0x0C
  h.cmd(0x00);
  h.cmd(0xC0);
  // Clear 0x01
  h.cmd(0x00);
  h.cmd(0x10);
  // Entry 0x06
  h.cmd(0x00);
  h.cmd(0x60);
  // Set DDRAM addr 0x80
  h.cmd(0x80);
  h.cmd(0x00);
  // Data 'A' (0x41) as nibbles
  h.data(0x40);
  h.data(0x10);
  CHECK(h.cellShowsCode(0, 0, 'A'), "4-bit write: cell(0,0) != 'A'");

  // Addr 0x10 = cols offset -> visible row 2 on the 16x4 fold
  h.cmd(0x90);
  h.cmd(0x00);
  h.data(0x40);
  h.data(0x20);  // 'B'
  CHECK(h.cellShowsCode(0, 2, 'B'), "4-bit write: cell(0,2) != 'B'");
}

void testTextBuffer() {
  printf("text buffer readout\n");
  Harness h(makeConfig(ConfigPreset::TEXT_1602));
  init8bit(h);

  uint16_t cols = 0, rows = 0;
  h.tap.getTextBufferSize(&cols, &rows);
  CHECK(cols == 16 && rows == 2, "text size %dx%d != 16x2", cols, rows);

  h.cmd(0x80);
  for (const char* p = "Hello"; *p; ++p) h.data(static_cast<uint8_t>(*p));
  h.cmd(0x80 | 0x40);
  for (const char* p = "World"; *p; ++p) h.data(static_cast<uint8_t>(*p));

  uint8_t text[80];
  memset(text, 0xAA, sizeof(text));
  uint32_t n = h.tap.readTextBuffer(0, 32, text);
  CHECK(n == 32, "readTextBuffer returned %u != 32", n);
  CHECK(memcmp(text, "Hello           ", 16) == 0, "row 0 mismatch");
  CHECK(memcmp(text + 16, "World           ", 16) == 0, "row 1 mismatch");

  // offset/size are honoured and clamped to the buffer
  n = h.tap.readTextBuffer(16, 5, text);
  CHECK(n == 5 && memcmp(text, "World", 5) == 0, "offset read mismatch");
  n = h.tap.readTextBuffer(30, 100, text);
  CHECK(n == 2, "clamped read returned %u != 2", n);
  n = h.tap.readTextBuffer(32, 4, text);
  CHECK(n == 0, "out-of-range offset returned %u != 0", n);

  // Display shift moves the readout window with the visible cells
  h.cmd(0x18);  // shift left: position 0 shows DDRAM addr 1
  n = h.tap.readTextBuffer(0, 16, text);
  CHECK(n == 16 && memcmp(text, "ello            ", 16) == 0,
        "shifted row 0 mismatch");
  h.cmd(0x02);  // return home

  // 1-line mode: the second row has no mapped cells and reads as spaces
  h.cmd(0x30);  // DL=1, N=0
  n = h.tap.readTextBuffer(16, 16, text);
  CHECK(n == 16, "1-line read returned %u != 16", n);
  bool allSpace = true;
  for (uint32_t i = 0; i < 16; ++i) allSpace &= (text[i] == 0x20);
  CHECK(allSpace, "1-line mode row 1 not blank");
}

void testTextBufferFourRow() {
  printf("text buffer 4-row fold\n");
  LcdTapConfig cfg = makeConfig(ConfigPreset::TEXT_2004);
  cfg.busInterface = BusType::I2C;
  Harness h(cfg);
  init8bit(h);

  uint16_t cols = 0, rows = 0;
  h.tap.getTextBufferSize(&cols, &rows);
  CHECK(cols == 20 && rows == 4, "text size %dx%d != 20x4", cols, rows);

  h.cmd(0x80 | 0x00);
  h.data('1');  // row 0
  h.cmd(0x80 | 0x40);
  h.data('2');  // row 1
  h.cmd(0x80 | 0x14);
  h.data('3');  // cols offset on line 1 -> visible row 2
  h.cmd(0x80 | 0x54);
  h.data('4');  // cols offset on line 2 -> visible row 3
  uint8_t text[80];
  uint32_t n = h.tap.readTextBuffer(0, 80, text);
  CHECK(n == 80, "readTextBuffer returned %u != 80", n);
  CHECK(text[0] == '1' && text[20] == '2' && text[40] == '3' && text[60] == '4',
        "fold rows mismatch: %c %c %c %c", text[0], text[20], text[40],
        text[60]);
}

void testTextBufferAbsent() {
  printf("text buffer absent on pixel controllers\n");
  Harness h(makeConfig(ConfigPreset::ST7789));
  uint16_t cols = 99, rows = 99;
  h.tap.getTextBufferSize(&cols, &rows);
  CHECK(cols == 0 && rows == 0, "pixel controller text size %dx%d != 0x0", cols,
        rows);
  uint8_t text[4];
  CHECK(h.tap.readTextBuffer(0, 4, text) == 0,
        "pixel controller readTextBuffer != 0");
}

void testDirtyTracking() {
  printf("dirty tracking\n");
  Harness h(makeConfig(ConfigPreset::TEXT_1602));
  init8bit(h);
  h.tap.setDirtyTracking(true);
  CHECK(h.tap.isDirtyTrackingActive(), "dirty tracking inactive");
  memset(h.tap.dirtyMap(), 0, h.cfg.buffHeight);

  h.cmd(0x80 | 0x02);  // cell (2,0): columns 12..17, all within segment 0
  h.data('A');
  const uint8_t* dm = h.tap.dirtyMap();
  for (int y = 0; y < h.cfg.buffHeight; ++y) {
    bool expectDirty = (y < static_cast<int>(CELL_H));
    CHECK((dm[y] != 0) == expectDirty, "dirty row %d: got 0x%02X", y, dm[y]);
    if (expectDirty) {
      CHECK(dm[y] == 0x01, "dirty row %d: expected seg bit0, got 0x%02X", y,
            dm[y]);
    }
  }
}

}  // namespace

int main() {
  testBasicWrite();
  testShift();
  testCgramOpr();
  testCursor();
  testDoubleHeight();
  testOneLineMode();
  testFourRowFold();
  testNibbleMode();
  testTextBuffer();
  testTextBufferFourRow();
  testTextBufferAbsent();
  testDirtyTracking();

  if (gFailures == 0) {
    printf("all tests passed\n");
    return 0;
  }
  printf("%d failure(s)\n", gFailures);
  return 1;
}
