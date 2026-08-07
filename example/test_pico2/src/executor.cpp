#include "testrig/executor.hpp"

#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"
#include "testrig/bus.hpp"
#include "testrig/config.h"
#include "testrig/ctrl_init.hpp"
#include "testrig/testgen.hpp"
#include "testrig/verify.hpp"

namespace testrig {

using lcdtap::BusType;
using lcdtap::ControllerFamily;
using lcdtap::InterfaceFormat;
using lcdtap::TrimMode;

namespace {

// Wire-format TX buffer, shared by all vectors. Frames larger than this
// (320x480 RGB666 = 460.8 KB) are generated and sent in row chunks inside
// one RAMWR.
uint8_t gTxBuf[TX_BUFFER_SIZE];

uint32_t gRunCounter = 0;

constexpr uint32_t CMD_TIMEOUT_MS = 3000;  // per-character
constexpr uint32_t FB_TIMEOUT_MS = 3000;   // per-character while streaming
constexpr int NUM_DUMMY_FRAMES = 10;

// True when the interface format cannot be selected by controller commands
// and must be forced via interfaceFormatOverride.
bool needsFormatOverride(ControllerFamily fam, InterfaceFormat fmt) {
  if (fam == ControllerFamily::SSD1331 &&
      fmt == InterfaceFormat::RGB666_UNPACK_RA8_BE) {
    return true;  // SETREMAP only encodes RGB332 / RGB565
  }
  if (fam == ControllerFamily::ILI9341 &&
      fmt == InterfaceFormat::RGB111_HPACK2_H2L_RA8) {
    return true;  // COLMOD 0x01 is not mapped for this family
  }
  return false;
}

// Value override for a config id, applied while echoing the preset values
// back as setparams.
bool overrideValue(const char* id, const TestVector& vec, ControllerFamily fam,
                   int32_t* out) {
  bool text = vectorIsText(vec);
  if (strcmp(id, "busInterface") == 0) {
    *out = static_cast<int32_t>(vec.busInterface);
    return true;
  }
  if (!text && strcmp(id, "buffWidth") == 0) {
    *out = vec.buffWidth;
    return true;
  }
  if (!text && strcmp(id, "buffHeight") == 0) {
    *out = vec.buffHeight;
    return true;
  }
  if (strcmp(id, "trimMode") == 0) {
    *out = static_cast<int32_t>(vec.trimMode);
    return true;
  }
  if (strcmp(id, "trimX") == 0) {
    *out = vec.trimX;
    return true;
  }
  if (strcmp(id, "trimY") == 0) {
    *out = vec.trimY;
    return true;
  }
  if (strcmp(id, "trimWidth") == 0) {
    *out = vec.trimWidth;
    return true;
  }
  if (strcmp(id, "trimHeight") == 0) {
    *out = vec.trimHeight;
    return true;
  }
  if (strcmp(id, "flipMode") == 0) {
    *out = 0;
    return true;
  }
  if (strcmp(id, "forcePwrOn") == 0) {
    *out = 0;
    return true;
  }
  if (strcmp(id, "intfFmtOvr") == 0) {
    *out = needsFormatOverride(fam, vec.interfaceFormat)
               ? static_cast<int32_t>(vec.interfaceFormat)
               : -1;
    return true;
  }
  if (strcmp(id, "outputRot") == 0) {
    *out = vec.outputRot;
    return true;
  }
  if (strcmp(id, "outputInterface") == 0) {
    *out = 0;  // DVI-D: never provokes a reboot
    return true;
  }
  return false;
}

bool statsAreClean(JsonDocument& doc) {
  static const char* const CHECKED[] = {"RX Drop", "RX HW Overflow",
                                        "Unknown Commands"};
  for (JsonObject e : doc["stats"].as<JsonArray>()) {
    const char* name = e["name"];
    if (name == nullptr) continue;
    for (const char* c : CHECKED) {
      if (strcmp(name, c) == 0 && e["value"].as<uint32_t>() != 0) {
        return false;
      }
    }
  }
  return true;
}

struct FrameRect {
  uint16_t x, y, w, h;
};

// Window/pattern rectangle: AUTO trim converges to the addressed window, so
// AUTO vectors send only the trim rectangle; everything else sends the full
// framebuffer.
FrameRect frameRect(const TestVector& vec) {
  if (vec.trimMode == TrimMode::AUTO) {
    return {vec.trimX, vec.trimY, vec.trimWidth, vec.trimHeight};
  }
  return {0, 0, vec.buffWidth, vec.buffHeight};
}

// Readback source region the target will report.
FrameRect readbackRect(const TestVector& vec) {
  if (vec.trimMode == TrimMode::OFF) {
    return {0, 0, vec.buffWidth, vec.buffHeight};
  }
  return {vec.trimX, vec.trimY, vec.trimWidth, vec.trimHeight};
}

void sendPatternFrame(ControllerFamily fam, const TestVector& vec,
                      const FrameRect& r, uint32_t seed) {
  PatternParams pat{vec.interfaceFormat, vec.buffWidth, seed};
  size_t rowBytes = wireBytesPerRow(vec.interfaceFormat, r.w);
  bool gray1 = (vec.interfaceFormat == InterfaceFormat::GRAY1_VPACK8_H2L);
  uint16_t rowStep = gray1 ? 8 : 1;  // GRAY1 "rowBytes" covers one page

  uint16_t chunkRows =
      static_cast<uint16_t>(sizeof(gTxBuf) / rowBytes) * rowStep;
  if (chunkRows == 0) return;

  ctrlBeginFrame(fam, vec, r.x, r.y, r.w, r.h);
  for (uint16_t y = 0; y < r.h;) {
    uint16_t rows = static_cast<uint16_t>(r.h - y);
    if (rows > chunkRows) rows = chunkRows;
    size_t n = buildWireRect(pat, r.x, static_cast<uint16_t>(r.y + y), r.w,
                             rows, gTxBuf);
    busWriteData(gTxBuf, n);
    y = static_cast<uint16_t>(y + rows);
  }
}

void sendDummyFrame(ControllerFamily fam, const TestVector& vec,
                    const FrameRect& r) {
  static uint8_t rowBuf[1024];
  size_t rowBytes = buildSolidRow(vec.interfaceFormat, r.w, rowBuf);
  if (rowBytes == 0 || rowBytes > sizeof(rowBuf)) return;
  bool gray1 = (vec.interfaceFormat == InterfaceFormat::GRAY1_VPACK8_H2L);
  uint16_t lines = gray1 ? static_cast<uint16_t>(r.h / 8) : r.h;

  ctrlBeginFrame(fam, vec, r.x, r.y, r.w, r.h);
  for (uint16_t y = 0; y < lines; y++) {
    busWriteData(rowBuf, rowBytes);
  }
}

}  // namespace

bool executorRunVector(const TestVector& vec, JsonClient& client,
                       ExecResult* res, ExecProgressFn progress, void* ctx) {
  *res = ExecResult{};
  ControllerFamily fam = presetFamily(vec.preset);
  const bool text = vectorIsText(vec);
  const uint32_t seed =
      hash32(0x54455354u ^ (gRunCounter++ << 8) ^
             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&vec)));

  auto fail = [&](const char* stage) {
    res->stage = stage;
    busDeselect();
    return false;
  };
  auto step = [&](int pct) {
    return progress == nullptr || progress(ctx, pct);
  };

  if (!step(0)) return fail("cancel");

  // --- 1. getparams for the base preset -----------------------------------
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "{\"command\":\"getparams\",\"preset\":\"%s\"}",
           lcdtap::CONFIG_PRESET_NAMES[static_cast<int>(vec.preset)]);
  JsonDocument params;
  if (!client.command(cmd, params, CMD_TIMEOUT_MS) ||
      params["params"].isNull()) {
    return fail("getparams");
  }

  // Values the verifier / bus layer needs from the preset.
  bool rbSwap = false;
  uint8_t i2cAddr = 0x3C;
  uint16_t textCols = 0, textRows = 0;

  // --- 2. build setparams: preset values + vector overrides ---------------
  static char setBuf[1536];
  int pos = snprintf(setBuf, sizeof(setBuf),
                     "{\"command\":\"setparams\",\"params\":{\"save\":false");
  for (JsonObject item : params["params"].as<JsonArray>()) {
    const char* id = item["id"];
    if (id == nullptr || strcmp(id, "compositeDac") == 0) continue;
    int32_t value = item["value"].as<int32_t>();  // booleans become 0/1
    if (strcmp(id, "swapRB") == 0) rbSwap = (value != 0);
    if (strcmp(id, "i2cAddr") == 0) i2cAddr = static_cast<uint8_t>(value);
    if (strcmp(id, "textCols") == 0) textCols = static_cast<uint16_t>(value);
    if (strcmp(id, "textRows") == 0) {
      // The wire value is the ENUM index into {"1", "2", "4"}, not the
      // actual row count.
      textRows = (value >= 2) ? 4 : (value >= 1) ? 2 : 1;
    }
    int32_t ovr;
    if (overrideValue(id, vec, fam, &ovr)) value = ovr;
    pos += snprintf(setBuf + pos, sizeof(setBuf) - pos, ",\"%s\":%ld", id,
                    static_cast<long>(value));
    if (pos >= static_cast<int>(sizeof(setBuf)) - 32) {
      return fail("setparams-size");
    }
  }
  snprintf(setBuf + pos, sizeof(setBuf) - pos, "}}");

  JsonDocument resp;
  if (!client.command(setBuf, resp, CMD_TIMEOUT_MS) ||
      strcmp(resp["response"] | "", "ok") != 0) {
    return fail("setparams");
  }
  sleep_ms(300);  // let the target re-init its bus input
  if (!step(10)) return fail("cancel");

  if (!client.command("{\"command\":\"statsreset\"}", resp, CMD_TIMEOUT_MS)) {
    return fail("statsreset");
  }

  // --- 3. bus + reset + controller init -----------------------------------
  if (!busSelect(vec.busInterface, vec.busFreqHz, i2cAddr,
                 fam == ControllerFamily::ST7032)) {
    return fail("bus-select");
  }
  busResetPulse(10, 120);
  ctrlInitDisplay(fam, vec, textRows);
  if (!step(20)) return fail("cancel");

  // --- 4. frames -----------------------------------------------------------
  if (text) {
    if (textCols == 0 || textRows == 0) return fail("text-geometry");
    char line[64];
    for (int f = 0; f < 2; f++) {  // dummy text
      for (uint16_t row = 0; row < textRows; row++) {
        ctrlSetTextRow(row, textCols);
        memset(line, '#', textCols);
        busWriteData(reinterpret_cast<uint8_t*>(line), textCols);
      }
    }
    for (uint16_t row = 0; row < textRows; row++) {  // compare text
      ctrlSetTextRow(row, textCols);
      buildTextRow(seed, row, textCols, line);
      busWriteData(reinterpret_cast<uint8_t*>(line), textCols);
    }
  } else {
    FrameRect r = frameRect(vec);
    for (int f = 0; f < NUM_DUMMY_FRAMES; f++) {
      sendDummyFrame(fam, vec, r);
      if (!step(20 + 5 * f)) return fail("cancel");
    }
    sendPatternFrame(fam, vec, r, seed);
  }
  if (!step(75)) return fail("cancel");

  // --- 5. read back and verify --------------------------------------------
  if (text) {
    uint16_t cols = 0, rows = 0;
    uint8_t data[160];
    if (!client.getTextBuffer(&cols, &rows, data, sizeof(data),
                              CMD_TIMEOUT_MS)) {
      return fail("gettextbuffer");
    }
    if (cols != textCols || rows != textRows) {
      res->stage = "text-dims";
      res->got = cols;
      res->want = textCols;
      busDeselect();
      return false;
    }
    res->mismatchCount = textMismatchCount(seed, cols, rows, data);
    if (res->mismatchCount != 0) {
      res->stage = "text-compare";
      busDeselect();
      return false;
    }
  } else {
    FrameRect rb = readbackRect(vec);
    VerifyParams vp;
    vp.pattern = PatternParams{vec.interfaceFormat, vec.buffWidth, seed};
    vp.rot = vec.outputRot;
    vp.rbSwap = rbSwap;
    vp.srcX = rb.x;
    vp.srcY = rb.y;
    vp.srcW = rb.w;
    vp.srcH = rb.h;
    FrameVerifier v;
    v.begin(vp);
    if (!client.getFramebuffer(v, true, FB_TIMEOUT_MS)) {
      return fail("getframebuffer");
    }
    if (!v.finish()) {
      res->stage = "compare";
      res->mismatchCount = v.mismatchCount();
      res->badX = v.firstBadX();
      res->badY = v.firstBadY();
      res->got = v.firstGot();
      res->want = v.firstWant();
      busDeselect();
      return false;
    }
  }
  if (!step(90)) return fail("cancel");

  // --- 6. stats must be clean ---------------------------------------------
  if (!client.command("{\"command\":\"getstats\"}", resp, CMD_TIMEOUT_MS)) {
    return fail("getstats");
  }
  if (!statsAreClean(resp)) return fail("stats");

  busDeselect();
  step(100);
  res->pass = true;
  return true;
}

}  // namespace testrig
