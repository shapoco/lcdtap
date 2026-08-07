#include "testrig/ui.hpp"

#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"
#include "testrig/executor.hpp"
#include "testrig/input.hpp"
#include "testrig/oled.hpp"
#include "testrig/usb_host.hpp"
#include "testrig/vectors.hpp"
#include "tusb.h"

namespace testrig {

using lcdtap::BusType;
using lcdtap::ControllerFamily;
using lcdtap::InterfaceFormat;
using lcdtap::TrimMode;

namespace {

enum class UiMode : uint8_t {
  VECTOR_SELECT,
  ITEM_SELECT,
  VALUE_SELECT,
  RUNNING,
  RESULT,
  MSC_MODE,
};

// Customizable items (order fixed; text vectors only expose the first two).
enum class Item : uint8_t { INTF, FREQ, RESO, FMT, ROT, TRIM, COUNT };
const char* const ITEM_NAMES[] = {"Intf", "Freq", "Reso", "Fmt", "Rot", "Trim"};

JsonClient* gClient = nullptr;

UiMode gMode = UiMode::VECTOR_SELECT;
UiMode gModeBeforeMsc = UiMode::VECTOR_SELECT;

int gVectorSel = 0;  // 0 = ALL, 1..N = TEST_VECTORS[i-1]
TestVector gCustom;  // working copy of the selected vector
int gItemSel = 0;
int gValueSel = 0;  // candidate index while in VALUE_SELECT

// Results of the last run.
bool gRan[64];
ExecResult gResults[64];
int gRunCount = 0;  // vectors attempted in the last run
int gFailCount = 0;
int gFailSel = 0;  // cursor into the failed list
int gRunProgress = 0;
const char* gRunName = "";
volatile bool gCancelReq = false;

bool gDirty = true;
bool gBlinkPhase = false;
absolute_time_t gNextBlink;

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

void fmtFreq(uint32_t hz, char* buf, size_t cap) {
  if (hz >= 1000000) {
    if (hz % 1000000 == 0) {
      snprintf(buf, cap, "%luMHz", static_cast<unsigned long>(hz / 1000000));
    } else {
      snprintf(buf, cap, "%lu.%luMHz", static_cast<unsigned long>(hz / 1000000),
               static_cast<unsigned long>((hz / 100000) % 10));
    }
  } else {
    snprintf(buf, cap, "%lukHz", static_cast<unsigned long>(hz / 1000));
  }
}

const char* fmtName(InterfaceFormat f) {
  return lcdtap::INTERFACE_FORMAT_NAMES[static_cast<int>(f) + 1];
}

int itemCount(const TestVector& v) {
  return vectorIsText(v) ? 2 : static_cast<int>(Item::COUNT);
}

void applyTrimRule(TestVector* v) {
  if (v->trimMode == TrimMode::OFF) {
    v->trimX = v->trimY = v->trimWidth = v->trimHeight = 0;
  } else {
    v->trimX = 11;
    v->trimY = 15;
    v->trimWidth = static_cast<uint16_t>(v->buffWidth / 2);
    v->trimHeight = static_cast<uint16_t>(v->buffHeight / 2);
  }
}

void loadVector(int sel) {
  if (sel >= 1) gCustom = TEST_VECTORS[sel - 1];
}

// ---------------------------------------------------------------------------
// Value editing
// ---------------------------------------------------------------------------

int valueChoiceCount(const TestVector& v, Item item) {
  ControllerFamily fam = presetFamily(v.preset);
  uint32_t f[4];
  uint16_t w[4], h[4];
  InterfaceFormat fmts[4];
  BusType buses[4];
  switch (item) {
    case Item::INTF: return allowedBuses(fam, buses, 4);
    case Item::FREQ: return freqChoices(v.busInterface, f, 4);
    case Item::RESO: return resolutionChoices(fam, w, h, 4);
    case Item::FMT: return formatChoices(fam, fmts, 4);
    case Item::ROT: return 4;
    case Item::TRIM: return 3;
    default: return 0;
  }
}

// Index of the vector's current value in the item's choice list.
int valueCurrentIndex(const TestVector& v, Item item) {
  ControllerFamily fam = presetFamily(v.preset);
  switch (item) {
    case Item::INTF: {
      BusType buses[4];
      int n = allowedBuses(fam, buses, 4);
      for (int i = 0; i < n; i++) {
        if (buses[i] == v.busInterface) return i;
      }
      return 0;
    }
    case Item::FREQ: {
      uint32_t f[4];
      int n = freqChoices(v.busInterface, f, 4);
      for (int i = 0; i < n; i++) {
        if (f[i] == v.busFreqHz) return i;
      }
      return 2;
    }
    case Item::RESO: {
      uint16_t w[4], h[4];
      int n = resolutionChoices(fam, w, h, 4);
      for (int i = 0; i < n; i++) {
        if (w[i] == v.buffWidth && h[i] == v.buffHeight) return i;
      }
      return 0;
    }
    case Item::FMT: {
      InterfaceFormat fmts[4];
      int n = formatChoices(fam, fmts, 4);
      for (int i = 0; i < n; i++) {
        if (fmts[i] == v.interfaceFormat) return i;
      }
      return 0;
    }
    case Item::ROT: return v.outputRot;
    case Item::TRIM: return static_cast<int>(v.trimMode);
    default: return 0;
  }
}

void valueApply(TestVector* v, Item item, int idx) {
  ControllerFamily fam = presetFamily(v->preset);
  switch (item) {
    case Item::INTF: {
      BusType buses[4];
      allowedBuses(fam, buses, 4);
      v->busInterface = buses[idx];
      v->busFreqHz = defaultFreq(v->busInterface);
      break;
    }
    case Item::FREQ: {
      uint32_t f[4];
      freqChoices(v->busInterface, f, 4);
      v->busFreqHz = f[idx];
      break;
    }
    case Item::RESO: {
      uint16_t w[4], h[4];
      resolutionChoices(fam, w, h, 4);
      v->buffWidth = w[idx];
      v->buffHeight = h[idx];
      applyTrimRule(v);
      break;
    }
    case Item::FMT: {
      InterfaceFormat fmts[4];
      formatChoices(fam, fmts, 4);
      v->interfaceFormat = fmts[idx];
      break;
    }
    case Item::ROT: v->outputRot = static_cast<uint8_t>(idx); break;
    case Item::TRIM:
      v->trimMode = static_cast<TrimMode>(idx);
      applyTrimRule(v);
      break;
    default: break;
  }
}

// Value display string for choice idx (or the current value with idx < 0).
void valueString(const TestVector& v, Item item, int idx, char* buf,
                 size_t cap) {
  TestVector tmp = v;
  if (idx >= 0) valueApply(&tmp, item, idx);
  switch (item) {
    case Item::INTF:
      snprintf(buf, cap, "%s",
               lcdtap::BUS_SHORT_NAMES[static_cast<int>(tmp.busInterface)]);
      break;
    case Item::FREQ: fmtFreq(tmp.busFreqHz, buf, cap); break;
    case Item::RESO:
      if (vectorIsText(tmp)) {
        snprintf(buf, cap, "n/a");
      } else {
        snprintf(buf, cap, "%ux%u", tmp.buffWidth, tmp.buffHeight);
      }
      break;
    case Item::FMT:
      snprintf(buf, cap, "%s",
               vectorIsText(tmp) ? "n/a" : fmtName(tmp.interfaceFormat));
      break;
    case Item::ROT:
      snprintf(buf, cap, "%s deg", lcdtap::ROTATION_NAMES[tmp.outputRot & 3]);
      break;
    case Item::TRIM:
      if (tmp.trimMode == TrimMode::OFF) {
        snprintf(buf, cap, "None");
      } else {
        snprintf(buf, cap, "%s %u,%u %ux%u",
                 tmp.trimMode == TrimMode::AUTO ? "Auto" : "Cust", tmp.trimX,
                 tmp.trimY, tmp.trimWidth, tmp.trimHeight);
      }
      break;
    default: buf[0] = '\0'; break;
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Draw the vector info screen. blinkItem: -1 = blink the title (vector
// select), 0.. = the item whose NAME blinks, blinkValueOf: item whose VALUE
// blinks (candidate gValueSel), -1 = none.
void drawVectorScreen(int blinkItem, int blinkValueOf) {
  char line[24], val[20];
  oledClear();

  bool hideTitle = (blinkItem == -1 && gBlinkPhase);
  if (!hideTitle) {
    if (gVectorSel == 0) {
      snprintf(line, sizeof(line), "< ALL (%d) >", NUM_TEST_VECTORS);
    } else {
      snprintf(line, sizeof(line), "< #%d %s >", gVectorSel, gCustom.name);
    }
    oledText(0, 0, line);
  }

  if (gVectorSel == 0) {
    oledText(0, 2, "Run all vectors");
    oledText(0, 6, "Start: run");
    oledFlush();
    return;
  }

  snprintf(line, sizeof(line), "Preset: %s",
           lcdtap::CONFIG_PRESET_NAMES[static_cast<int>(gCustom.preset)]);
  oledText(0, 1, line);

  struct Row {
    Item item;
    int col, row;
  };
  static const Row ROWS[] = {
      {Item::INTF, 0, 2}, {Item::FREQ, 0, 3}, {Item::RESO, 0, 4},
      {Item::FMT, 0, 5},  {Item::ROT, 0, 6},  {Item::TRIM, 0, 7},
  };
  int items = itemCount(gCustom);
  for (int i = 0; i < static_cast<int>(sizeof(ROWS) / sizeof(ROWS[0])); i++) {
    Item it = ROWS[i].item;
    if (static_cast<int>(it) >= items) continue;
    bool nameBlinks = (blinkItem == static_cast<int>(it)) && gBlinkPhase;
    bool valueBlinks = (blinkValueOf == static_cast<int>(it)) && gBlinkPhase;
    int candidate = (blinkValueOf == static_cast<int>(it)) ? gValueSel : -1;
    valueString(gCustom, it, candidate, val, sizeof(val));
    snprintf(line, sizeof(line), "%s%-4s: %s", nameBlinks ? "    " : "",
             nameBlinks ? "" : ITEM_NAMES[static_cast<int>(it)],
             valueBlinks ? "" : val);
    oledText(ROWS[i].col, ROWS[i].row, line);
  }
  oledFlush();
}

void drawRunning() {
  char line[24];
  oledClear();
  oledText(0, 0, "Running...");
  snprintf(line, sizeof(line), "%s", gRunName);
  oledText(0, 2, line);
  snprintf(line, sizeof(line), "%3d%%", gRunProgress);
  oledBigText(0, 4, line);
  oledText(0, 7, "Back: cancel");
  oledFlush();
}

void drawResult() {
  char line[24];
  oledClear();
  if (gFailCount == 0) {
    oledBigText(0, 0, "OK");
    snprintf(line, sizeof(line), "%d vector(s) passed", gRunCount);
    oledText(0, 3, line);
  } else {
    oledBigText(0, 0, "NG");
    snprintf(line, sizeof(line), "%d/%d failed", gFailCount, gRunCount);
    oledText(6, 1, line);
    // Failed list around the cursor.
    int shown = 0;
    int idx = 0;
    for (int i = 0; i < NUM_TEST_VECTORS && shown < 4; i++) {
      if (!gRan[i] || gResults[i].pass) continue;
      if (idx >= gFailSel - 1) {
        bool cursor = (idx == gFailSel);
        snprintf(line, sizeof(line), "%c%s", cursor ? '>' : ' ',
                 TEST_VECTORS[i].name);
        oledText(0, 3 + shown, line);
        shown++;
      }
      idx++;
    }
    // Detail of the selected failure.
    const ExecResult* r = nullptr;
    idx = 0;
    for (int i = 0; i < NUM_TEST_VECTORS; i++) {
      if (!gRan[i] || gResults[i].pass) continue;
      if (idx == gFailSel) {
        r = &gResults[i];
        break;
      }
      idx++;
    }
    if (r != nullptr) {
      if (r->mismatchCount != 0) {
        snprintf(line, sizeof(line), "%s n=%lu (%u,%u)", r->stage,
                 static_cast<unsigned long>(r->mismatchCount), r->badX,
                 r->badY);
      } else {
        snprintf(line, sizeof(line), "stage: %s", r->stage);
      }
      oledText(0, 7, line);
    }
  }
  oledFlush();
}

void drawMscMode() {
  oledClear();
  if (!gBlinkPhase) {
    oledBigText(0, 2, "Download");
    oledBigText(2, 4, "Mode");
  }
  oledFlush();
}

void draw() {
  switch (gMode) {
    case UiMode::VECTOR_SELECT: drawVectorScreen(-1, -1); break;
    case UiMode::ITEM_SELECT: drawVectorScreen(gItemSel, -1); break;
    case UiMode::VALUE_SELECT: drawVectorScreen(-2, gItemSel); break;
    case UiMode::RUNNING: drawRunning(); break;
    case UiMode::RESULT: drawResult(); break;
    case UiMode::MSC_MODE: drawMscMode(); break;
  }
}

// ---------------------------------------------------------------------------
// Test execution (blocking, pumps USB + cancel inside the progress callback)
// ---------------------------------------------------------------------------

int gRunBasePct = 0;
int gRunSpanPct = 100;

bool runProgress(void* /*ctx*/, int pct) {
  gRunProgress = gRunBasePct + pct * gRunSpanPct / 100;
  drawRunning();
  tud_task();
  InputEvent ev;
  while ((ev = inputPoll()) != InputEvent::NONE) {
    if (ev == InputEvent::KEY_BACK) gCancelReq = true;
  }
  return !gCancelReq;
}

void runTests() {
  memset(gRan, 0, sizeof(gRan));
  gFailCount = 0;
  gFailSel = 0;
  gCancelReq = false;
  gRunProgress = 0;
  gMode = UiMode::RUNNING;

  if (gVectorSel == 0) {
    gRunCount = NUM_TEST_VECTORS;
    for (int i = 0; i < NUM_TEST_VECTORS && !gCancelReq; i++) {
      gRunName = TEST_VECTORS[i].name;
      gRunBasePct = i * 100 / NUM_TEST_VECTORS;
      gRunSpanPct = 100 / NUM_TEST_VECTORS;
      gRan[i] = true;
      executorRunVector(TEST_VECTORS[i], *gClient, &gResults[i], runProgress,
                        nullptr);
      if (!gResults[i].pass) gFailCount++;
      printf("[%s] %s%s\n", TEST_VECTORS[i].name,
             gResults[i].pass ? "PASS" : "FAIL ", gResults[i].stage);
    }
  } else {
    gRunCount = 1;
    int i = gVectorSel - 1;
    gRunName = gCustom.name;
    gRunBasePct = 0;
    gRunSpanPct = 100;
    gRan[i] = true;
    executorRunVector(gCustom, *gClient, &gResults[i], runProgress, nullptr);
    if (!gResults[i].pass) gFailCount++;
    printf("[%s] %s%s n=%lu\n", gCustom.name,
           gResults[i].pass ? "PASS" : "FAIL ", gResults[i].stage,
           static_cast<unsigned long>(gResults[i].mismatchCount));
  }

  inputFlush();
  gMode = UiMode::RESULT;
  gDirty = true;
}

// Vector-table index of the gFailSel-th failure, or -1.
int failVectorIndex() {
  int idx = 0;
  for (int i = 0; i < NUM_TEST_VECTORS; i++) {
    if (!gRan[i] || gResults[i].pass) continue;
    if (idx == gFailSel) return i;
    idx++;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Input handling per mode
// ---------------------------------------------------------------------------

void handleEvent(InputEvent ev) {
  switch (gMode) {
    case UiMode::VECTOR_SELECT:
      switch (ev) {
        case InputEvent::KEY_INC:
          gVectorSel = (gVectorSel + 1) % (NUM_TEST_VECTORS + 1);
          loadVector(gVectorSel);
          break;
        case InputEvent::KEY_DEC:
          gVectorSel = (gVectorSel + NUM_TEST_VECTORS) % (NUM_TEST_VECTORS + 1);
          loadVector(gVectorSel);
          break;
        case InputEvent::KEY_SELECT:
          if (gVectorSel != 0) {
            gItemSel = 0;
            gMode = UiMode::ITEM_SELECT;
          }
          break;
        case InputEvent::KEY_START: runTests(); break;
        default: break;
      }
      break;

    case UiMode::ITEM_SELECT: {
      int n = itemCount(gCustom);
      switch (ev) {
        case InputEvent::KEY_INC: gItemSel = (gItemSel + 1) % n; break;
        case InputEvent::KEY_DEC: gItemSel = (gItemSel + n - 1) % n; break;
        case InputEvent::KEY_SELECT:
          gValueSel = valueCurrentIndex(gCustom, static_cast<Item>(gItemSel));
          gMode = UiMode::VALUE_SELECT;
          break;
        case InputEvent::KEY_BACK: gMode = UiMode::VECTOR_SELECT; break;
        case InputEvent::KEY_START: runTests(); break;
        default: break;
      }
      break;
    }

    case UiMode::VALUE_SELECT: {
      int n = valueChoiceCount(gCustom, static_cast<Item>(gItemSel));
      if (n <= 0) n = 1;
      switch (ev) {
        case InputEvent::KEY_INC: gValueSel = (gValueSel + 1) % n; break;
        case InputEvent::KEY_DEC: gValueSel = (gValueSel + n - 1) % n; break;
        case InputEvent::KEY_SELECT:
          valueApply(&gCustom, static_cast<Item>(gItemSel), gValueSel);
          gMode = UiMode::ITEM_SELECT;
          break;
        case InputEvent::KEY_BACK: gMode = UiMode::ITEM_SELECT; break;
        case InputEvent::KEY_START: runTests(); break;
        default: break;
      }
      break;
    }

    case UiMode::RESULT:
      switch (ev) {
        case InputEvent::KEY_INC:
          if (gFailCount > 0) gFailSel = (gFailSel + 1) % gFailCount;
          break;
        case InputEvent::KEY_DEC:
          if (gFailCount > 0) {
            gFailSel = (gFailSel + gFailCount - 1) % gFailCount;
          }
          break;
        case InputEvent::KEY_SELECT:
          if (gFailCount > 0) {
            int i = failVectorIndex();
            if (i >= 0) {
              gVectorSel = i + 1;
              loadVector(gVectorSel);
            }
            gItemSel = 0;
            gMode = UiMode::ITEM_SELECT;
          } else {
            gMode = UiMode::VECTOR_SELECT;
          }
          break;
        case InputEvent::KEY_BACK: {
          if (gFailCount > 0) {
            int i = failVectorIndex();
            if (i >= 0) {
              gVectorSel = i + 1;
              loadVector(gVectorSel);
            }
          }
          gMode = UiMode::VECTOR_SELECT;
          break;
        }
        case InputEvent::KEY_START: runTests(); break;
        default: break;
      }
      break;

    default: break;
  }
}

}  // namespace

void uiInit(JsonClient* client) {
  gClient = client;
  gVectorSel = 0;
  loadVector(1);  // have a valid gCustom even while ALL is selected
  gNextBlink = make_timeout_time_ms(500);
  gDirty = true;
}

void uiTick() {
  // Download-mode overlay (never during a run; RUNNING is blocking anyway).
  bool msc = usbHostMscMounted();
  if (gMode != UiMode::MSC_MODE && msc) {
    gModeBeforeMsc = gMode;
    gMode = UiMode::MSC_MODE;
    inputFlush();
    gDirty = true;
  } else if (gMode == UiMode::MSC_MODE && !msc) {
    gMode = gModeBeforeMsc;
    inputFlush();
    gDirty = true;
  }

  InputEvent ev;
  while ((ev = inputPoll()) != InputEvent::NONE) {
    if (gMode == UiMode::MSC_MODE) continue;  // ignored
    handleEvent(ev);
    gDirty = true;
  }

  // Blink phase drives the blinking title/name/value and the MSC screen.
  if (absolute_time_diff_us(get_absolute_time(), gNextBlink) <= 0) {
    gNextBlink = make_timeout_time_ms(500);
    gBlinkPhase = !gBlinkPhase;
    if (gMode != UiMode::RUNNING && gMode != UiMode::RESULT) gDirty = true;
    if (gMode == UiMode::MSC_MODE) gDirty = true;
  }

  if (gDirty) {
    gDirty = false;
    draw();
  }
}

}  // namespace testrig
