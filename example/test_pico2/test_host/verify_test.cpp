// Closed-loop host test for the test rig's generator + verifier against the
// REAL target code: wire streams built by testgen are fed into an actual
// lcdtap::LcdTap instance through the same command sequences the rig will
// send on hardware, the framebuffer is then serialized exactly like
// pico2_common json_intf.cpp getframebuffer does (fbIndexTrimmed walk ->
// RGB565-RLE -> Base64, using the target's own rleEncodeSegment/b64Encode3),
// and the stream is drained through FrameVerifier. Covers every interface
// format, all four rotations, trim OFF/CUSTOM/AUTO, R/B swap and the
// character LCD text path, plus a corruption negative test.
//
// Build & run:
//   g++ -O2 -Wall -Wextra -I../include -I../../../lib/include
//       -I../../pico2_common/include -o /tmp/testrig_verify_test
//       verify_test.cpp ../src/testgen.cpp ../src/verify.cpp
//       ../../pico2_common/src/json_rle.cpp ../../pico2_common/src/json_b64.cpp
//       ../../../lib/src/lcdtap.cpp ../../../lib/src/config.cpp
//       ../../../lib/src/spi_display_base.cpp
//       ../../../lib/src/st7789_controller.cpp
//       ../../../lib/src/ili9341_controller.cpp
//       ../../../lib/src/ssd1306_controller.cpp
//       ../../../lib/src/ssd1331_controller.cpp
//       ../../../lib/src/st7032_controller.cpp
//       ../../../lib/src/ks0108_controller.cpp
//   /tmp/testrig_verify_test

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lcdtap/devices/ili9341.hpp"
#include "lcdtap/devices/ssd1306.hpp"
#include "lcdtap/devices/ssd1331.hpp"
#include "lcdtap/devices/st7032.hpp"
#include "lcdtap/devices/st7789.hpp"
#include "lcdtap/lcdtap.hpp"
#include "lcdtap/pico2/json_b64.hpp"
#include "lcdtap/pico2/json_rle.hpp"
#include "testrig/verify.hpp"

using namespace lcdtap;
using namespace testrig;

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

static void* testAlloc(size_t size) { return malloc(size); }
static void testFree(void* ptr) { free(ptr); }

// Verbatim copy of the output->physical mapping in
// example/pico2_common/src/json_intf.cpp (fbIndexTrimmed).
static uint32_t fbIndexTrimmed(uint16_t dx, uint16_t dy, uint16_t srcX,
                               uint16_t srcY, uint16_t srcW, uint16_t srcH,
                               uint16_t physW, uint8_t rot) {
  uint32_t bx, by;
  switch (rot) {
    default:
    case 0:
      bx = srcX + dx;
      by = srcY + dy;
      break;
    case 1:
      bx = srcX + dy;
      by = srcY + srcH - 1u - dx;
      break;
    case 2:
      bx = srcX + srcW - 1u - dx;
      by = srcY + srcH - 1u - dy;
      break;
    case 3:
      bx = srcX + srcW - 1u - dy;
      by = srcY + dx;
      break;
  }
  return by * physW + bx;
}

// Serialize the framebuffer the way json_intf.cpp getframebuffer does and
// return the Base64 payload. Also reports the source region used.
static std::string serializeFb(LcdTap& tap, uint16_t* srcX, uint16_t* srcY,
                               uint16_t* srcW, uint16_t* srcH) {
  LcdTapConfig cfg = tap.getConfig();
  uint16_t physW = cfg.buffWidth;
  uint16_t physH = cfg.buffHeight;
  uint16_t sx, sy, sw, sh;
  tap.getOutSrcRegion(&sx, &sy, &sw, &sh);
  if (sw == 0 || sh == 0) {
    sx = 0;
    sy = 0;
    sw = physW;
    sh = physH;
  }
  *srcX = sx;
  *srcY = sy;
  *srcW = sw;
  *srcH = sh;
  uint8_t rot = cfg.outputRotation & 3u;
  uint16_t outW = (rot & 1u) ? sh : sw;
  uint16_t outH = (rot & 1u) ? sw : sh;
  const uint16_t* fb = tap.getFramebuf();
  const bool inv = tap.isOutputInverted();

  std::vector<uint8_t> rle;
  uint16_t seg[RLE_SEG_MAX_PIXELS];
  uint8_t pkt[RLE_SEG_MAX_BYTES];
  for (uint16_t oy = 0; oy < outH; oy++) {
    for (uint16_t ox = 0; ox < outW;) {
      int n = outW - ox;
      if (n > RLE_SEG_MAX_PIXELS) n = RLE_SEG_MAX_PIXELS;
      for (int i = 0; i < n; i++) {
        uint16_t px = fb[fbIndexTrimmed(static_cast<uint16_t>(ox + i), oy, sx,
                                        sy, sw, sh, physW, rot)];
        if (inv) px ^= 0xFFFFu;
        seg[i] = px;
      }
      int len = rleEncodeSegment(seg, n, pkt);
      rle.insert(rle.end(), pkt, pkt + len);
      ox = static_cast<uint16_t>(ox + n);
    }
  }

  std::string b64;
  size_t i = 0;
  char quad[4];
  for (; i + 3 <= rle.size(); i += 3) {
    b64Encode3(&rle[i], quad);
    b64.append(quad, 4);
  }
  if (i < rle.size()) {
    b64EncodePad(&rle[i], static_cast<int>(rle.size() - i), quad);
    b64.append(quad, 4);
  }
  return b64;
}

struct Harness {
  LcdTap tap;

  explicit Harness(const LcdTapConfig& cfg)
      : tap(cfg, HostInterface{testAlloc, testFree, nullptr, nullptr}) {
    if (tap.getStatus() != Status::OK) {
      printf("  FATAL: LcdTap init failed\n");
      exit(1);
    }
    memset(tap.getFramebuf(), 0,
           (size_t)cfg.buffWidth * cfg.buffHeight * sizeof(uint16_t));
  }

  void cmd(uint8_t c) { tap.inputCommand(c); }
  void cmd(uint8_t c, std::initializer_list<uint8_t> params) {
    tap.inputCommand(c);
    for (uint8_t p : params) tap.inputCommand(p);
  }
  void cmdData(uint8_t c, std::initializer_list<uint8_t> params) {
    tap.inputCommand(c);
    for (uint8_t p : params) {
      uint8_t b = p;
      tap.inputData(&b, 1);
    }
  }
  void data(const uint8_t* d, size_t n) {
    tap.inputData(d, static_cast<uint32_t>(n));
  }
};

// Run serialization + verification and check the result.
static void runVerify(LcdTap& tap, const PatternParams& pat, bool rbSwap,
                      const char* what) {
  uint16_t sx, sy, sw, sh;
  std::string b64 = serializeFb(tap, &sx, &sy, &sw, &sh);
  VerifyParams vp;
  vp.pattern = pat;
  vp.rot = tap.getConfig().outputRotation & 3u;
  vp.rbSwap = rbSwap;
  vp.srcX = sx;
  vp.srcY = sy;
  vp.srcW = sw;
  vp.srcH = sh;
  FrameVerifier v;
  v.begin(vp);
  for (char c : b64) v.feedBase64(c);
  if (!v.finish()) {
    CHECK(false,
          "%s: mismatches=%u pixels=%u err=%d first=(%u,%u) got=%04X "
          "want=%04X",
          what, v.mismatchCount(), v.pixelCount(), (int)v.streamError(),
          v.firstBadX(), v.firstBadY(), v.firstGot(), v.firstWant());
  }
}

// ---------------------------------------------------------------------------
// ST7789: COLMOD-selected formats x rotations, trim OFF
// ---------------------------------------------------------------------------
static void testSt7789Formats() {
  printf("testSt7789Formats\n");
  struct {
    uint8_t colmod;
    InterfaceFormat fmt;
  } kCases[] = {
      {0x03, InterfaceFormat::RGB444_HPACK2_H2L_BE},
      {0x05, InterfaceFormat::RGB565_BE},
      {0x06, InterfaceFormat::RGB666_UNPACK_LA8_BE},
  };
  for (auto& cs : kCases) {
    for (uint8_t rot = 0; rot < 4; rot++) {
      LcdTapConfig cfg;
      getDefaultConfig(ControllerFamily::ST7789, &cfg);
      cfg.buffWidth = 60;
      cfg.buffHeight = 80;
      cfg.outputRotation = rot;
      Harness h(cfg);
      h.cmd(st7789::CMD_SWRESET);
      h.cmdData(st7789::CMD_MADCTL, {0x00});
      h.cmdData(st7789::CMD_COLMOD, {cs.colmod});
      h.cmdData(st7789::CMD_CASET, {0, 0, 0, 59});
      h.cmdData(st7789::CMD_RASET, {0, 0, 0, 79});
      h.cmd(st7789::CMD_RAMWR);

      PatternParams pat{cs.fmt, 60, 0x1000u + rot};
      std::vector<uint8_t> wire(wireBytesForRect(cs.fmt, 60, 80));
      buildWireRect(pat, 0, 0, 60, 80, wire.data());
      h.data(wire.data(), wire.size());

      char what[64];
      snprintf(what, sizeof(what), "ST7789 colmod=%02X rot=%u", cs.colmod, rot);
      runVerify(h.tap, pat, cfg.swapRB, what);
    }
  }
}

// ---------------------------------------------------------------------------
// ST7789: CUSTOM trim (full-frame send, region-only compare) x rotations
// ---------------------------------------------------------------------------
static void testSt7789TrimCustom() {
  printf("testSt7789TrimCustom\n");
  for (uint8_t rot = 0; rot < 4; rot++) {
    LcdTapConfig cfg;
    getDefaultConfig(ControllerFamily::ST7789, &cfg);
    cfg.buffWidth = 60;
    cfg.buffHeight = 80;
    cfg.outputRotation = rot;
    cfg.trimMode = TrimMode::CUSTOM;
    cfg.trimX = 11;
    cfg.trimY = 15;
    cfg.trimWidth = 30;
    cfg.trimHeight = 40;
    Harness h(cfg);
    h.cmd(st7789::CMD_SWRESET);
    h.cmdData(st7789::CMD_MADCTL, {0x00});
    h.cmdData(st7789::CMD_COLMOD, {0x05});
    h.cmdData(st7789::CMD_CASET, {0, 0, 0, 59});
    h.cmdData(st7789::CMD_RASET, {0, 0, 0, 79});
    h.cmd(st7789::CMD_RAMWR);

    PatternParams pat{InterfaceFormat::RGB565_BE, 60, 0x2000u + rot};
    std::vector<uint8_t> wire(wireBytesForRect(pat.format, 60, 80));
    buildWireRect(pat, 0, 0, 60, 80, wire.data());
    h.data(wire.data(), wire.size());

    uint16_t sx, sy, sw, sh;
    h.tap.getOutSrcRegion(&sx, &sy, &sw, &sh);
    CHECK(sx == 11 && sy == 15 && sw == 30 && sh == 40,
          "custom trim region %u,%u %ux%u", sx, sy, sw, sh);

    char what[64];
    snprintf(what, sizeof(what), "ST7789 trimC rot=%u", rot);
    runVerify(h.tap, pat, cfg.swapRB, what);
  }
}

// ---------------------------------------------------------------------------
// ST7789: AUTO trim — windowed send only; the trim region must converge to
// the addressed window (AUTO expands from CASET/RASET, not pixel content)
// ---------------------------------------------------------------------------
static void testSt7789TrimAuto() {
  printf("testSt7789TrimAuto\n");
  for (uint8_t rot = 0; rot < 4; rot++) {
    LcdTapConfig cfg;
    getDefaultConfig(ControllerFamily::ST7789, &cfg);
    cfg.buffWidth = 60;
    cfg.buffHeight = 80;
    cfg.outputRotation = rot;
    cfg.trimMode = TrimMode::AUTO;
    Harness h(cfg);
    h.cmd(st7789::CMD_SWRESET);
    h.cmdData(st7789::CMD_MADCTL, {0x00});
    h.cmdData(st7789::CMD_COLMOD, {0x05});
    h.cmdData(st7789::CMD_CASET, {0, 11, 0, 11 + 29});
    h.cmdData(st7789::CMD_RASET, {0, 15, 0, 15 + 39});
    h.cmd(st7789::CMD_RAMWR);

    PatternParams pat{InterfaceFormat::RGB565_BE, 60, 0x3000u + rot};
    std::vector<uint8_t> wire(wireBytesForRect(pat.format, 30, 40));
    buildWireRect(pat, 11, 15, 30, 40, wire.data());
    h.data(wire.data(), wire.size());

    uint16_t sx, sy, sw, sh;
    h.tap.getOutSrcRegion(&sx, &sy, &sw, &sh);
    CHECK(sx == 11 && sy == 15 && sw == 30 && sh == 40,
          "auto trim region %u,%u %ux%u (rot=%u)", sx, sy, sw, sh, rot);

    char what[64];
    snprintf(what, sizeof(what), "ST7789 trimA rot=%u", rot);
    runVerify(h.tap, pat, cfg.swapRB, what);
  }
}

// ---------------------------------------------------------------------------
// ILI9341 family (covers ILI9488 vectors): RGB111 needs
// interfaceFormatOverride — COLMOD 0x01 is not mapped for this family.
// ---------------------------------------------------------------------------
static void testIli9341Formats() {
  printf("testIli9341Formats\n");
  struct {
    int8_t ovr;
    uint8_t colmod;  // 0 = do not send
    InterfaceFormat fmt;
  } kCases[] = {
      {static_cast<int8_t>(InterfaceFormat::RGB111_HPACK2_H2L_RA8), 0,
       InterfaceFormat::RGB111_HPACK2_H2L_RA8},
      {-1, 0x05, InterfaceFormat::RGB565_BE},
      {-1, 0x06, InterfaceFormat::RGB666_UNPACK_LA8_BE},
  };
  for (auto& cs : kCases) {
    for (uint8_t rot = 0; rot < 2; rot++) {
      LcdTapConfig cfg;
      getDefaultConfig(ControllerFamily::ILI9341, &cfg);
      cfg.buffWidth = 64;
      cfg.buffHeight = 48;
      cfg.outputRotation = rot;
      cfg.interfaceFormatOverride = cs.ovr;
      Harness h(cfg);
      h.cmd(ili9341::CMD_SWRESET);
      h.cmdData(ili9341::CMD_MADCTL, {0x00});
      if (cs.colmod != 0) h.cmdData(ili9341::CMD_COLMOD, {cs.colmod});
      h.cmdData(ili9341::CMD_CASET, {0, 0, 0, 63});
      h.cmdData(ili9341::CMD_RASET, {0, 0, 0, 47});
      h.cmd(ili9341::CMD_RAMWR);

      PatternParams pat{cs.fmt, 64, 0x4000u + rot};
      std::vector<uint8_t> wire(wireBytesForRect(cs.fmt, 64, 48));
      buildWireRect(pat, 0, 0, 64, 48, wire.data());
      h.data(wire.data(), wire.size());

      char what[64];
      snprintf(what, sizeof(what), "ILI9341 fmt=%d rot=%u", (int)cs.fmt, rot);
      runVerify(h.tap, pat, cfg.swapRB, what);
    }
  }
}

// ---------------------------------------------------------------------------
// SSD1331: SETREMAP-selected RGB332/RGB565 and override-selected RGB666RA
// ---------------------------------------------------------------------------
static void testSsd1331Formats() {
  printf("testSsd1331Formats\n");
  struct {
    int8_t ovr;
    uint8_t remapDepth;
    InterfaceFormat fmt;
  } kCases[] = {
      {-1, ssd1331::REMAP_COLOR_DEPTH_256, InterfaceFormat::RGB332},
      {-1, ssd1331::REMAP_COLOR_DEPTH_65K, InterfaceFormat::RGB565_BE},
      {static_cast<int8_t>(InterfaceFormat::RGB666_UNPACK_RA8_BE),
       ssd1331::REMAP_COLOR_DEPTH_65K, InterfaceFormat::RGB666_UNPACK_RA8_BE},
  };
  for (auto& cs : kCases) {
    for (uint8_t rot = 0; rot < 4; rot++) {
      LcdTapConfig cfg;
      getDefaultConfig(ControllerFamily::SSD1331, &cfg);
      cfg.outputRotation = rot;
      cfg.interfaceFormatOverride = cs.ovr;
      Harness h(cfg);
      const uint16_t w = cfg.buffWidth;     // 96
      const uint16_t hgt = cfg.buffHeight;  // 64
      h.cmd(ssd1331::CMD_SETREMAP, {cs.remapDepth});
      h.cmd(ssd1331::CMD_SETCOLUMN, {0, static_cast<uint8_t>(w - 1)});
      h.cmd(ssd1331::CMD_SETROW, {0, static_cast<uint8_t>(hgt - 1)});

      PatternParams pat{cs.fmt, w, 0x5000u + rot};
      std::vector<uint8_t> wire(wireBytesForRect(cs.fmt, w, hgt));
      buildWireRect(pat, 0, 0, w, hgt, wire.data());
      h.data(wire.data(), wire.size());

      char what[64];
      snprintf(what, sizeof(what), "SSD1331 fmt=%d rot=%u", (int)cs.fmt, rot);
      runVerify(h.tap, pat, cfg.swapRB, what);
    }
  }
}

// ---------------------------------------------------------------------------
// SSD1306: GRAY1, horizontal addressing mode
// ---------------------------------------------------------------------------
static void testSsd1306() {
  printf("testSsd1306\n");
  for (uint8_t rot = 0; rot < 4; rot++) {
    LcdTapConfig cfg;
    getDefaultConfig(ControllerFamily::SSD1306, &cfg);
    cfg.outputRotation = rot;
    Harness h(cfg);
    const uint16_t w = cfg.buffWidth;     // 128
    const uint16_t hgt = cfg.buffHeight;  // 64
    h.cmd(ssd1306::CMD_SEG_REMAP_0);
    h.cmd(ssd1306::CMD_COM_SCAN_INC);
    h.cmd(ssd1306::CMD_SET_ADDR_MODE, {0x00});  // horizontal
    h.cmd(ssd1306::CMD_SET_COL_ADDR, {0, static_cast<uint8_t>(w - 1)});
    h.cmd(ssd1306::CMD_SET_PAGE_ADDR, {0, static_cast<uint8_t>(hgt / 8 - 1)});

    PatternParams pat{InterfaceFormat::GRAY1_VPACK8_H2L, w, 0x6000u + rot};
    std::vector<uint8_t> wire(
        wireBytesForRect(InterfaceFormat::GRAY1_VPACK8_H2L, w, hgt));
    buildWireRect(pat, 0, 0, w, hgt, wire.data());
    h.data(wire.data(), wire.size());

    char what[64];
    snprintf(what, sizeof(what), "SSD1306 rot=%u", rot);
    runVerify(h.tap, pat, cfg.swapRB, what);
  }
}

// ---------------------------------------------------------------------------
// ST7032: text pattern round trip through the DDRAM path
// ---------------------------------------------------------------------------
static void testSt7032Text() {
  printf("testSt7032Text\n");
  LcdTapConfig cfg;
  getDefaultConfig(ControllerFamily::ST7032, &cfg);
  cfg.textCols = 16;
  cfg.textRows = 2;
  Harness h(cfg);
  h.cmd(st7032::CMD_FUNCTION_SET | st7032::FUNC_DL | st7032::FUNC_N);
  h.cmd(st7032::CMD_DISPLAY_ONOFF | st7032::DISPLAY_D);
  h.cmd(st7032::CMD_CLEAR_DISPLAY);

  const uint32_t seed = 0x7000u;
  static const uint8_t kRowAddr[] = {0x00, 0x40};
  for (uint16_t row = 0; row < 2; row++) {
    h.cmd(st7032::CMD_SET_DDRAM_ADDR | kRowAddr[row]);
    char line[16];
    buildTextRow(seed, row, 16, line);
    h.data(reinterpret_cast<const uint8_t*>(line), 16);
  }

  uint16_t cols, rows;
  h.tap.getTextBufferSize(&cols, &rows);
  CHECK(cols == 16 && rows == 2, "text size %ux%u", cols, rows);
  uint8_t text[32];
  uint32_t n = h.tap.readTextBuffer(0, sizeof(text), text);
  CHECK(n == 32, "readTextBuffer n=%u", n);
  uint32_t bad = textMismatchCount(seed, cols, rows, text);
  CHECK(bad == 0, "text mismatches=%u", bad);
}

// ---------------------------------------------------------------------------
// Negative: a corrupted pixel must be detected at the right location
// ---------------------------------------------------------------------------
static void testCorruptionDetected() {
  printf("testCorruptionDetected\n");
  LcdTapConfig cfg;
  getDefaultConfig(ControllerFamily::ST7789, &cfg);
  cfg.buffWidth = 60;
  cfg.buffHeight = 80;
  cfg.outputRotation = 1;
  Harness h(cfg);
  h.cmd(st7789::CMD_SWRESET);
  h.cmdData(st7789::CMD_MADCTL, {0x00});
  h.cmdData(st7789::CMD_COLMOD, {0x05});
  h.cmdData(st7789::CMD_CASET, {0, 0, 0, 59});
  h.cmdData(st7789::CMD_RASET, {0, 0, 0, 79});
  h.cmd(st7789::CMD_RAMWR);

  PatternParams pat{InterfaceFormat::RGB565_BE, 60, 0x8000u};
  std::vector<uint8_t> wire(wireBytesForRect(pat.format, 60, 80));
  buildWireRect(pat, 0, 0, 60, 80, wire.data());
  h.data(wire.data(), wire.size());

  h.tap.getFramebuf()[42 * 60 + 17] ^= 0x5555u;

  uint16_t sx, sy, sw, sh;
  std::string b64 = serializeFb(h.tap, &sx, &sy, &sw, &sh);
  VerifyParams vp;
  vp.pattern = pat;
  vp.rot = 1;
  vp.rbSwap = cfg.swapRB;
  vp.srcX = sx;
  vp.srcY = sy;
  vp.srcW = sw;
  vp.srcH = sh;
  FrameVerifier v;
  v.begin(vp);
  for (char c : b64) v.feedBase64(c);
  CHECK(!v.finish(), "corruption must be detected");
  CHECK(v.mismatchCount() == 1, "mismatchCount=%u", v.mismatchCount());
  CHECK(v.firstBadX() == 17 && v.firstBadY() == 42, "first bad (%u,%u)",
        v.firstBadX(), v.firstBadY());
}

int main() {
  testSt7789Formats();
  testSt7789TrimCustom();
  testSt7789TrimAuto();
  testIli9341Formats();
  testSsd1331Formats();
  testSsd1306();
  testSt7032Text();
  testCorruptionDetected();
  if (gFailures == 0) {
    printf("ALL TESTS PASSED\n");
    return 0;
  }
  printf("%d FAILURE(S)\n", gFailures);
  return 1;
}
