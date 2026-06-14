#include <lcdtap/font8x16.hpp>
#include <lcdtap/osd.hpp>

#include <cstdio>
#include <cstring>

namespace lcdtap {

//=============================================================================
// File-scope lookup tables
//=============================================================================

static const char* kControllerNames[] = {"ST7789", "SSD1306", "SSD1331"};
static constexpr int kNumControllers = 3;

static const char* kOnOffNames[] = {"OFF", "ON"};

static const char* kRotationNames[] = {"0", "90", "180", "270"};

static const char* kScaleModeNames[] = {"STRETCH", "FIT", "PIXEL_PERF"};
static constexpr int kNumScaleModes = 3;

static const char*
    kIfFmtOvrNames[1 + static_cast<int>(InterfaceFormat::NUM_FORMATS)];
static bool kIfFmtOvrNamesInited = false;

//=============================================================================
// getDefaultOsdConfig
//=============================================================================

void getDefaultOsdConfig(OsdConfig* cfg) {
  cfg->onMenuOpen = nullptr;
  cfg->onActionActivated = nullptr;
  cfg->userData = nullptr;
}

//=============================================================================
// Osd::init
//=============================================================================

void Osd::init(const OsdConfig& cfg) {
  cfg_ = cfg;
  numItems_ = 0;
  selectedItem_ = 0;
  scrollOffset_ = 0;
  state_ = OsdState::HIDDEN;
  blinkOn_ = true;
  lastInput_ = 0;
  keyPressMs_ = 0;
  lastRepeatMs_ = 0;
  lastBlinkMs_ = 0;
  dumpScrollOffset_ = 0;
  lastDumpState_ = DumpState::WAIT;
  lastDumpSize_ = 0;
  dumpViewDirty_ = true;
  memset(textBuf_, ' ', sizeof(textBuf_));
  memset(textCol_, 0, sizeof(textCol_));
}

//=============================================================================
// Osd::update
//=============================================================================

uint8_t Osd::update(uint64_t nowMs, LcdTap& lcdtap, uint8_t input) {
  // Update blink state
  if (nowMs - lastBlinkMs_ >= BLINK_PERIOD_MS) {
    blinkOn_ = !blinkOn_;
    lastBlinkMs_ = nowMs;
  }

  // Determine which keys fire this frame (new press or held repeat)
  uint8_t activeKeys = 0;
  uint8_t newKeys = static_cast<uint8_t>(input & ~lastInput_);
  if (newKeys) {
    activeKeys = newKeys;
    keyPressMs_ = nowMs;
    lastRepeatMs_ = nowMs;
  } else if (input && (nowMs - keyPressMs_ >= KEY_REPEAT_DELAY_MS) &&
             (nowMs - lastRepeatMs_ >= KEY_REPEAT_PERIOD_MS)) {
    activeKeys = input;
    lastRepeatMs_ = nowMs;
  }
  lastInput_ = input;

  uint8_t action = OSD_ACTION_NONE;

  if (state_ == OsdState::HIDDEN) {
    // Quick rotation: Left/Right cycle output rotation without opening OSD
    if (activeKeys & (OSD_KEY_LEFT | OSD_KEY_RIGHT)) {
      LcdTapConfig cfg = lcdtap.getConfig();
      if (activeKeys & OSD_KEY_LEFT) {
        cfg.outputRotation = (cfg.outputRotation + 3u) & 3u;
      } else {
        cfg.outputRotation = (cfg.outputRotation + 1u) & 3u;
      }
      lcdtap.setOutputRotation(cfg.outputRotation);
      action = OSD_ACTION_APPLY;
    }
    // Show OSD on Enter
    if (activeKeys & OSD_KEY_ENTER) {
      initMenuItems(lcdtap);
      if (cfg_.onMenuOpen) cfg_.onMenuOpen(this, cfg_.userData);
      state_ = OsdState::MAIN_MENU;
      blinkOn_ = true;
      lastBlinkMs_ = nowMs;
    }
  } else if (state_ == OsdState::MAIN_MENU) {
    // Navigate up/down
    if (activeKeys & OSD_KEY_UP) {
      selectedItem_ =
          (selectedItem_ == 0) ? (numItems_ - 1) : (selectedItem_ - 1);
      updateScroll();
    }
    if (activeKeys & OSD_KEY_DOWN) {
      selectedItem_ =
          (selectedItem_ == numItems_ - 1) ? 0 : (selectedItem_ + 1);
      updateScroll();
    }

    OsdMenuItem& sel = items_[selectedItem_];

    if (sel.type == OsdMenuType::ACTION) {
      if (activeKeys & OSD_KEY_ENTER) {
        if (sel.id == OSD_ITEM_ID_VIEW_DUMP) {
          state_ = OsdState::DUMP_VIEW;
          dumpScrollOffset_ = 0;
          lastDumpState_ = lcdtap.dumpGetState();
          lastDumpSize_ = lcdtap.dumpGetSize();
          renderDumpView(lcdtap);
        } else {
          bool stayOpen = false;
          if (cfg_.onActionActivated) {
            stayOpen =
                cfg_.onActionActivated(this, &sel, lcdtap, cfg_.userData);
          }
          if (!stayOpen) {
            action = static_cast<uint8_t>(sel.value);
            if (action == OSD_ACTION_APPLY) {
              applyConfig(lcdtap);
            }
            state_ = OsdState::HIDDEN;
          }
        }
      }
    } else {
      // Adjust value with left/right
      if (activeKeys & OSD_KEY_LEFT) {
        switch (sel.type) {
          case OsdMenuType::INTEGER:
            if (sel.value - sel.step >= sel.min)
              sel.value = static_cast<int16_t>(sel.value - sel.step);
            else
              sel.value = sel.min;
            break;
          case OsdMenuType::BOOL: sel.value = (~sel.value) & 1; break;
          case OsdMenuType::ENUM:
            sel.value = static_cast<int16_t>(sel.value - sel.step);
            if (sel.value < sel.min) sel.value = sel.max;
            break;
          default: break;
        }
      }
      if (activeKeys & OSD_KEY_RIGHT) {
        switch (sel.type) {
          case OsdMenuType::INTEGER:
            if (sel.value + sel.step <= sel.max)
              sel.value = static_cast<int16_t>(sel.value + sel.step);
            else
              sel.value = sel.max;
            break;
          case OsdMenuType::BOOL: sel.value = (~sel.value) & 1; break;
          case OsdMenuType::ENUM:
            sel.value = static_cast<int16_t>(sel.value + sel.step);
            if (sel.value > sel.max) sel.value = sel.min;
            break;
          default: break;
        }
      }
      if (activeKeys & OSD_KEY_ENTER) {
        setSelectedIndex(getItemIndexById(OSD_ITEM_ID_APPLY));
        renderAll();
      }
    }

    if (state_ == OsdState::MAIN_MENU) renderAll();
  } else if (state_ == OsdState::DUMP_VIEW) {
    bool needsRender = dumpViewDirty_;

    if (activeKeys & OSD_KEY_LEFT) {
      state_ = OsdState::MAIN_MENU;
      renderAll();
      return OSD_ACTION_NONE;
    }
    if (activeKeys & OSD_KEY_ENTER) {
      lcdtap.dumpStart(getDefaultDumpConfig());
      lcdtap.dumpForceTrigger();
      needsRender = true;
    }
    if (activeKeys & OSD_KEY_RIGHT) {
      lcdtap.dumpAbort();
      needsRender = true;
    }
    if (activeKeys & OSD_KEY_UP) {
      if (dumpScrollOffset_ > 0) {
        --dumpScrollOffset_;
        needsRender = true;
      }
    }
    if (activeKeys & OSD_KEY_DOWN) {
      constexpr int MAX_SCROLL = LcdTap::DUMP_BUFFER_SIZE / 16 - (ROWS - 3);
      if (dumpScrollOffset_ < MAX_SCROLL) {
        ++dumpScrollOffset_;
        needsRender = true;
      }
    }

    DumpState curState = lcdtap.dumpGetState();
    uint16_t curSize = lcdtap.dumpGetSize();
    if (curState != lastDumpState_ || curSize != lastDumpSize_) {
      needsRender = true;
      lastDumpState_ = curState;
      lastDumpSize_ = curSize;
    }
    if (needsRender) {
      renderDumpView(lcdtap);
      dumpViewDirty_ = false;
    }
  }

  return action;
}

//=============================================================================
// Osd::fillScanline
//=============================================================================

void Osd::fillScanline(uint16_t line, uint16_t* dst) const {
  if (state_ == OsdState::HIDDEN || line >= static_cast<uint16_t>(OSD_HEIGHT))
    return;

  const int textRow = line >> 4;   // line / GLYPH_HEIGHT (16)
  const int pixRow = line & 0xFu;  // line % GLYPH_HEIGHT

  const char* rowPtr = &textBuf_[textRow * COLS];
  const uint8_t* colPtr = &textCol_[textRow * COLS];
  const uint8_t codeFirst = static_cast<uint8_t>(font8x16::CODE_FIRST);
  const uint8_t codeLast = static_cast<uint8_t>(font8x16::CODE_LAST);

  for (int col = 0; col < COLS; ++col) {
    uint8_t ch = static_cast<uint8_t>(rowPtr[col]);
    if (ch < codeFirst || ch > codeLast) ch = static_cast<uint8_t>(' ');
    const uint8_t colByte = colPtr[col];
    const uint16_t fg = OSD_PALETTE[colByte >> 4];
    const uint16_t bg = OSD_PALETTE[colByte & 0xFu];
    const uint8_t bits =
        font8x16::bitmap[static_cast<uint32_t>(ch - codeFirst) *
                             font8x16::GLYPH_HEIGHT +
                         static_cast<uint32_t>(pixRow)];
    uint16_t* d = dst + (col << 3);  // col * GLYPH_WIDTH (8)
    for (int j = 0; j < font8x16::GLYPH_WIDTH; ++j) {
      d[j] = ((bits >> j) & 1u) ? fg : bg;
    }
  }
}

//=============================================================================
// Osd::initMenuItems
//=============================================================================

void Osd::initMenuItems(const LcdTap& lcdtap) {
  LcdTapConfig cfg = lcdtap.getConfig();
  numItems_ = 0;

  if (!kIfFmtOvrNamesInited) {
    kIfFmtOvrNames[0] = "Off";
    for (int i = 0; i < static_cast<int>(InterfaceFormat::NUM_FORMATS); ++i) {
      kIfFmtOvrNames[i + 1] =
          getShortInterfaceFormatName(static_cast<InterfaceFormat>(i));
    }
    kIfFmtOvrNamesInited = true;
  }

  // Helper lambda to append an item and assign its ID
  auto add = [this](uint16_t id) -> OsdMenuItem& {
    OsdMenuItem& it = items_[numItems_++];
    it.id = id;
    return it;
  };

  // Controller Type
  {
    OsdMenuItem& it = add(OSD_ITEM_ID_CONTROLLER);
    it.type = OsdMenuType::ENUM;
    it.name = "Controller Type";
    it.unit = "";
    it.options = kControllerNames;
    it.min = 0;
    it.max = static_cast<int16_t>(kNumControllers - 1);
    it.step = 1;
    it.value = static_cast<int16_t>(cfg.controller);
  }

  // LCD Width
  {
    OsdMenuItem& it = add(OSD_ITEM_ID_LCD_WIDTH);
    it.type = OsdMenuType::INTEGER;
    it.name = "LCD Width";
    it.unit = "px";
    it.options = nullptr;
    it.min = 32;
    it.max = 480;
    it.step = 8;
    it.value = static_cast<int16_t>(cfg.lcdWidth);
  }

  // LCD Height
  {
    OsdMenuItem& it = add(OSD_ITEM_ID_LCD_HEIGHT);
    it.type = OsdMenuType::INTEGER;
    it.name = "LCD Height";
    it.unit = "px";
    it.options = nullptr;
    it.min = 32;
    it.max = 480;
    it.step = 8;
    it.value = static_cast<int16_t>(cfg.lcdHeight);
  }

  // Inversion
  {
    OsdMenuItem& it = add(OSD_ITEM_ID_INVERSION);
    it.type = OsdMenuType::BOOL;
    it.name = "Inversion";
    it.unit = "";
    it.options = kOnOffNames;
    it.min = 0;
    it.max = 1;
    it.step = 1;
    it.value = cfg.inverted ? 1 : 0;
  }

  // Swap Red/Blue
  {
    OsdMenuItem& it = add(OSD_ITEM_ID_SWAP_RB);
    it.type = OsdMenuType::BOOL;
    it.name = "Swap Red/Blue";
    it.unit = "";
    it.options = kOnOffNames;
    it.min = 0;
    it.max = 1;
    it.step = 1;
    it.value = cfg.swapRB ? 1 : 0;
  }

  // Force Power On
  {
    OsdMenuItem& it = add(OSD_ITEM_ID_FORCE_PWR_ON);
    it.type = OsdMenuType::BOOL;
    it.name = "Force Power On";
    it.unit = "";
    it.options = kOnOffNames;
    it.min = 0;
    it.max = 1;
    it.step = 1;
    it.value = cfg.forcePowerOn ? 1 : 0;
  }

  // Format Override
  {
    OsdMenuItem& it = add(OSD_ITEM_ID_IF_FMT_OVR);
    it.type = OsdMenuType::ENUM;
    it.name = "Format Override";
    it.unit = "";
    it.options = kIfFmtOvrNames;
    it.min = -1;
    it.max = static_cast<int16_t>(
        static_cast<int>(InterfaceFormat::NUM_FORMATS) - 1);
    it.step = 1;
    it.value = static_cast<int16_t>(cfg.interfaceFormatOverride);
  }

  // Output Rotation
  {
    OsdMenuItem& it = add(OSD_ITEM_ID_OUTPUT_ROT);
    it.type = OsdMenuType::ENUM;
    it.name = "Output Rotation";
    it.unit = "deg";
    it.options = kRotationNames;
    it.min = 0;
    it.max = 3;
    it.step = 1;
    it.value = static_cast<int16_t>(cfg.outputRotation & 3u);
  }

  // Output Scaling
  {
    OsdMenuItem& it = add(OSD_ITEM_ID_SCALE_MODE);
    it.type = OsdMenuType::ENUM;
    it.name = "Output Scaling";
    it.unit = "";
    it.options = kScaleModeNames;
    it.min = 0;
    it.max = static_cast<int16_t>(kNumScaleModes - 1);
    it.step = 1;
    it.value = static_cast<int16_t>(cfg.scaleMode);
  }

  // Command Dump
  {
    OsdMenuItem& it = add(OSD_ITEM_ID_VIEW_DUMP);
    it.type = OsdMenuType::ACTION;
    it.name = "Command Dump";
    it.unit = "";
    it.options = nullptr;
    it.min = 0;
    it.max = 0;
    it.step = 0;
    it.value = OSD_ACTION_NONE;
  }

  // Apply
  {
    OsdMenuItem& it = add(OSD_ITEM_ID_APPLY);
    it.type = OsdMenuType::ACTION;
    it.name = "Apply";
    it.unit = "";
    it.options = nullptr;
    it.min = 0;
    it.max = 0;
    it.step = 0;
    it.value = OSD_ACTION_APPLY;
  }

  // Cancel
  {
    OsdMenuItem& it = add(OSD_ITEM_ID_CANCEL);
    it.type = OsdMenuType::ACTION;
    it.name = "Cancel";
    it.unit = "";
    it.options = nullptr;
    it.min = 0;
    it.max = 0;
    it.step = 0;
    it.value = OSD_ACTION_CANCEL;
  }

  selectedItem_ = 0;
  scrollOffset_ = 0;
  renderAll();
}

//=============================================================================
// Osd::renderAll / renderTitle / renderItem
//=============================================================================

void Osd::renderAll() {
  renderTitle();
  constexpr int VISIBLE = ROWS - 1;
  for (int slot = 0; slot < VISIBLE; ++slot) {
    int idx = scrollOffset_ + slot;
    if (idx < numItems_) {
      renderItem(idx, slot + 1);
    } else {
      fillRow(slot + 1, ' ');
      fillRowColor(slot + 1,
                   static_cast<uint8_t>((PAL_WHITE << 4) | PAL_BLACK));
    }
  }
}

void Osd::renderTitle() { drawTitleBar("Configuration"); }

void Osd::renderItem(int idx, int row) {
  fillRow(row, ' ');

  const OsdMenuItem& item = items_[idx];
  const bool isSel = (idx == selectedItem_);

  fillRowColor(row, isSel
                        ? static_cast<uint8_t>((PAL_WHITE << 4) | PAL_SKY_BLUE)
                        : static_cast<uint8_t>((PAL_WHITE << 4) | PAL_BLACK));

  // Name field
  writeStr(row, COL_NAME_START, item.name, COL_NAME_END - COL_NAME_START + 1);

  // Separator
  writeChar(row, COL_SEP, ':');

  if (item.type == OsdMenuType::ACTION) {
    // "Hit Enter" centered in columns COL_IND_LEFT..COL_IND_RIGHT when
    // selected and blinking
    if (isSel && blinkOn_) {
      const char* label = "Hit Enter\x86";
      const int labelLen = 10;
      const int span = COL_IND_RIGHT - COL_IND_LEFT + 1;  // 15
      const int startCol = COL_IND_LEFT + (span - labelLen) / 2;
      writeStr(row, startCol, label, labelLen);
    }
  } else {
    // Indicators (blink when selected)
    if (isSel && blinkOn_) {
      writeChar(row, COL_IND_LEFT, '\x80');   // ◀
      writeChar(row, COL_IND_RIGHT, '\x81');  // ▶
    }

    // Value
    char valBuf[12];
    formatValue(valBuf, static_cast<int>(sizeof(valBuf)), item);
    writeStr(row, COL_VAL_START, valBuf, COL_VAL_END - COL_VAL_START + 1);

    // Unit
    if (item.unit && item.unit[0]) {
      writeStr(row, COL_UNIT_START, item.unit,
               COL_UNIT_END - COL_UNIT_START + 1);
    }
  }
}

//=============================================================================
// Osd::applyConfig / buildConfig
//=============================================================================

LcdTapConfig Osd::buildConfig() const {
  auto get = [this](uint16_t id, int16_t def) -> int16_t {
    const OsdMenuItem* it = nullptr;
    getItemById(id, &it);
    return it ? it->value : def;
  };

  ControllerType controller =
      static_cast<ControllerType>(get(OSD_ITEM_ID_CONTROLLER, 0));

  LcdTapConfig cfg;
  getDefaultConfig(controller, &cfg);

  cfg.lcdWidth = static_cast<uint16_t>(get(OSD_ITEM_ID_LCD_WIDTH, 240));
  cfg.lcdHeight = static_cast<uint16_t>(get(OSD_ITEM_ID_LCD_HEIGHT, 320));
  cfg.inverted = (get(OSD_ITEM_ID_INVERSION, 0) != 0);
  cfg.swapRB = (get(OSD_ITEM_ID_SWAP_RB, 0) != 0);
  // dviWidth / dviHeight: not set here; preserved by applyConfig()
  cfg.outputRotation =
      static_cast<uint8_t>(get(OSD_ITEM_ID_OUTPUT_ROT, 0) & 3u);
  cfg.scaleMode = static_cast<ScaleMode>(get(OSD_ITEM_ID_SCALE_MODE, 0));
  cfg.forcePowerOn = (get(OSD_ITEM_ID_FORCE_PWR_ON, 0) != 0);
  cfg.interfaceFormatOverride =
      static_cast<int8_t>(get(OSD_ITEM_ID_IF_FMT_OVR, -1));

  return cfg;
}

void Osd::applyConfig(LcdTap& lcdtap) const {
  LcdTapConfig newCfg = buildConfig();
  LcdTapConfig curCfg = lcdtap.getConfig();
  newCfg.dviWidth = curCfg.dviWidth;  // preserve current DVI resolution
  newCfg.dviHeight = curCfg.dviHeight;
  lcdtap.updateConfig(newCfg);
}

//=============================================================================
// Text helpers
//=============================================================================

void Osd::drawTitleBar(const char* title) {
  const int APP_NAME_LEN = 6;
  fillRow(0, ' ');
  setColorRange(0, COLS - APP_NAME_LEN, APP_NAME_LEN,
                static_cast<uint8_t>((PAL_BLACK << 4) | PAL_WHITE));
  setColorRange(0, COLS - APP_NAME_LEN - 4 * 1, 4,
                static_cast<uint8_t>((PAL_WHITE << 4) | PAL_CYAN));
  setColorRange(0, COLS - APP_NAME_LEN - 4 * 2, 4,
                static_cast<uint8_t>((PAL_CYAN << 4) | PAL_SKY_BLUE));
  setColorRange(0, COLS - APP_NAME_LEN - 4 * 3, 4,
                static_cast<uint8_t>((PAL_SKY_BLUE << 4) | PAL_BLUE));
  setColorRange(0, COLS - APP_NAME_LEN - 4 * 4, 4,
                static_cast<uint8_t>((PAL_BLUE << 4) | PAL_DARK_BLUE));
  setColorRange(0, 0, COLS - APP_NAME_LEN - 4 * 4,
                static_cast<uint8_t>((PAL_WHITE << 4) | PAL_DARK_BLUE));
  writeStr(0, COLS - APP_NAME_LEN - 4 * 4,
           " \x89\x8A\x8B \x89\x8A\x8B \x89\x8A\x8B \x89\x8A\x8B", 16);
  writeStr(0, 0, "\x87\x88", 2);
  writeStr(0, 3, title, COLS);
  writeStr(0, COLS - APP_NAME_LEN, "LcdTap", APP_NAME_LEN);
}

void Osd::fillRow(int row, char c) {
  memset(&textBuf_[row * COLS], c, static_cast<size_t>(COLS));
}

void Osd::fillRowColor(int row, uint8_t colByte) {
  memset(&textCol_[row * COLS], colByte, static_cast<size_t>(COLS));
}

void Osd::setColorRange(int row, int col, int len, uint8_t colByte,
                        uint8_t mask) {
  uint8_t* dst = &textCol_[row * COLS + col];
  for (int i = 0; i < len; ++i) {
    dst[i] = (dst[i] & ~mask) | (colByte & mask);
  }
}

void Osd::writeStr(int row, int col, const char* str, int maxLen) {
  char* dst = &textBuf_[row * COLS + col];
  int i = 0;
  while (str[i] && (maxLen < 0 || i < maxLen)) {
    dst[i] = str[i];
    ++i;
  }
}

void Osd::writeChar(int row, int col, char c) {
  textBuf_[row * COLS + col] = c;
}

void Osd::formatValue(char* buf, int bufLen, const OsdMenuItem& item) const {
  switch (item.type) {
    case OsdMenuType::BOOL:
    case OsdMenuType::ENUM:
      if (item.options && item.value >= item.min && item.value <= item.max) {
        snprintf(buf, static_cast<size_t>(bufLen), "%s",
                 item.options[item.value - item.min]);
      } else {
        snprintf(buf, static_cast<size_t>(bufLen), "---");
      }
      break;
    case OsdMenuType::INTEGER:
      snprintf(buf, static_cast<size_t>(bufLen), "%d",
               static_cast<int>(item.value));
      break;
    default: buf[0] = '\0'; break;
  }
}

//=============================================================================
// Osd::renderDumpView
//=============================================================================

void Osd::renderDumpView(LcdTap& lcdtap) {
  static const char HEX[] = "0123456789ABCDEF";

  // Clear background to avoid main-menu color bleed-through
  memset(textCol_, static_cast<uint8_t>((PAL_WHITE << 4) | PAL_BLACK),
         sizeof(textCol_));

  const DumpState dumpState = lcdtap.dumpGetState();
  const uint16_t dumpSize = lcdtap.dumpGetSize();
  const uint16_t* buf = lcdtap.dumpGetBuffer();

  // --- Row 0: title with colored state name ---
  drawTitleBar("Command Dump");
  const char* stateStr;
  uint8_t stateFg;
  switch (dumpState) {
    case DumpState::WAIT:
      stateStr = "Wait";
      stateFg = PAL_YELLOW;
      break;
    case DumpState::ACTIVE:
      stateStr = "Active";
      stateFg = PAL_RED;
      break;
    default:  // COMPLETE
      stateStr = "Complete";
      stateFg = PAL_CYAN;
      break;
  }
  int stateLen = 0;
  while (stateStr[stateLen]) ++stateLen;
  writeStr(0, 16, stateStr, stateLen);
  setColorRange(0, 16, stateLen, static_cast<uint8_t>(stateFg << 4), 0xF0);

  // --- Row 1: key hint ---
  fillRow(1, ' ');
  fillRowColor(1, static_cast<uint8_t>((PAL_WHITE << 4) | PAL_BLACK));
  writeStr(1, 0, "\x84:Back \x86:Trigger \x85:Abort \x82\x83:Scroll");

  // --- Row 2: column header ---
  fillRow(2, ' ');
  fillRowColor(2, static_cast<uint8_t>((PAL_SILVER << 4) | PAL_BLACK));
  writeStr(2, 0, "   +0+1+2+3+4+5+6+7+8+9+A+B+C+D+E+F");

  // --- Rows 3-14: data rows ---
  constexpr int HEADER_ROWS = 3;
  constexpr int VISIBLE_ROWS = ROWS - HEADER_ROWS;  // 12
  constexpr int ENTRIES_PER_ROW = 16;

  for (int slot = 0; slot < VISIBLE_ROWS; ++slot) {
    const int row = slot + HEADER_ROWS;
    const int rowIdx = dumpScrollOffset_ + slot;
    const int baseEntry = rowIdx * ENTRIES_PER_ROW;

    fillRow(row, ' ');

    // Address (e.g. "A0")
    writeChar(row, 0, HEX[rowIdx]);
    writeChar(row, 1, '0');
    setColorRange(row, 0, 3,
                  static_cast<uint8_t>((PAL_SILVER << 4) | PAL_BLACK));

    // 16 data columns
    for (int col = 0; col < ENTRIES_PER_ROW; ++col) {
      const int textCol = 3 + col * 2;
      const uint8_t altBg = (col & 1) ? PAL_DARK_GRAY : PAL_BLACK;

      const int entryIdx = baseEntry + col;
      if (entryIdx >= dumpSize) {
        // Empty slot
        writeChar(row, textCol, '.');
        writeChar(row, textCol + 1, '.');
        setColorRange(row, textCol, 2,
                      static_cast<uint8_t>((PAL_GRAY << 4) | altBg));
      } else {
        const uint16_t entry = buf[entryIdx];
        if (entry & 0x8000u) {
          // Special event
          if (entry == LcdTap::DUMP_EVENT_HW_RESET) {
            writeChar(row, textCol, 'H');
            writeChar(row, textCol + 1, 'R');
            setColorRange(row, textCol, 2,
                          static_cast<uint8_t>((PAL_BLACK << 4) | PAL_YELLOW));
          } else {
            writeChar(row, textCol, '?');
            writeChar(row, textCol + 1, '?');
            setColorRange(row, textCol, 2,
                          static_cast<uint8_t>((PAL_BLACK << 4) | PAL_RED));
          }
        } else if (entry & 0x100u) {
          // Data byte
          const uint8_t val = static_cast<uint8_t>(entry & 0xFFu);
          writeChar(row, textCol, HEX[val >> 4]);
          writeChar(row, textCol + 1, HEX[val & 0xFu]);
          setColorRange(row, textCol, 2,
                        static_cast<uint8_t>((PAL_WHITE << 4) | altBg));
        } else {
          // Command byte
          const uint8_t val = static_cast<uint8_t>(entry & 0xFFu);
          writeChar(row, textCol, HEX[val >> 4]);
          writeChar(row, textCol + 1, HEX[val & 0xFu]);
          setColorRange(row, textCol, 2,
                        static_cast<uint8_t>((PAL_BLACK << 4) | PAL_CYAN));
        }
      }
    }
  }
}

//=============================================================================
// Osd::updateScroll
//=============================================================================

void Osd::updateScroll() {
  constexpr int VISIBLE = ROWS - 1;
  if (selectedItem_ < scrollOffset_) scrollOffset_ = selectedItem_;
  if (selectedItem_ >= scrollOffset_ + VISIBLE)
    scrollOffset_ = selectedItem_ - VISIBLE + 1;
  int maxOffset = numItems_ - VISIBLE;
  if (maxOffset < 0) maxOffset = 0;
  if (scrollOffset_ > maxOffset) scrollOffset_ = maxOffset;
  if (scrollOffset_ < 0) scrollOffset_ = 0;
}

//=============================================================================
// Osd public item API
//=============================================================================

int Osd::getItemCount() const { return numItems_; }

int Osd::getSelectedIndex() const { return selectedItem_; }

void Osd::setSelectedIndex(int index) {
  if (numItems_ == 0) {
    selectedItem_ = 0;
    scrollOffset_ = 0;
    return;
  }
  if (index < 0) index = 0;
  if (index >= numItems_) index = numItems_ - 1;
  selectedItem_ = index;
  updateScroll();
}

int Osd::getItemIndexById(uint16_t id) const {
  for (int i = 0; i < numItems_; ++i) {
    if (items_[i].id == id) return i;
  }
  return -1;
}

void Osd::getItemByIndex(int index, const OsdMenuItem** item) const {
  *item = (index >= 0 && index < numItems_) ? &items_[index] : nullptr;
}

void Osd::getItemById(uint16_t id, const OsdMenuItem** item) const {
  getItemByIndex(getItemIndexById(id), item);
}

void Osd::insertItem(int index, const OsdMenuItem& item) {
  if (numItems_ >= MAX_ITEMS) return;
  // Negative index: -1 = append, -2 = before last, etc.
  if (index < 0) index = numItems_ + 1 + index;
  if (index < 0) index = 0;
  if (index > numItems_) index = numItems_;
  for (int i = numItems_; i > index; --i) items_[i] = items_[i - 1];
  items_[index] = item;
  ++numItems_;
  // Keep the same item visually selected after insertion
  if (index <= selectedItem_) {
    ++selectedItem_;
    updateScroll();
  }
}

void Osd::loadConfig(const LcdTapConfig& cfg) {
  auto set = [this](uint16_t id, int16_t value) {
    for (int i = 0; i < numItems_; ++i) {
      if (items_[i].id == id) {
        items_[i].value = value;
        return;
      }
    }
  };

  set(OSD_ITEM_ID_CONTROLLER, static_cast<int16_t>(cfg.controller));
  set(OSD_ITEM_ID_LCD_WIDTH, static_cast<int16_t>(cfg.lcdWidth));
  set(OSD_ITEM_ID_LCD_HEIGHT, static_cast<int16_t>(cfg.lcdHeight));
  set(OSD_ITEM_ID_INVERSION, cfg.inverted ? 1 : 0);
  set(OSD_ITEM_ID_SWAP_RB, cfg.swapRB ? 1 : 0);
  set(OSD_ITEM_ID_OUTPUT_ROT, static_cast<int16_t>(cfg.outputRotation & 3u));
  set(OSD_ITEM_ID_SCALE_MODE, static_cast<int16_t>(cfg.scaleMode));
  set(OSD_ITEM_ID_FORCE_PWR_ON, cfg.forcePowerOn ? 1 : 0);
  set(OSD_ITEM_ID_IF_FMT_OVR,
      static_cast<int16_t>(cfg.interfaceFormatOverride));

  renderAll();
}

void Osd::setItemValue(uint16_t id, int16_t value) {
  for (int i = 0; i < numItems_; ++i) {
    if (items_[i].id == id) {
      items_[i].value = value;
      int row = i - scrollOffset_ + 1;
      if (row >= 1 && row < ROWS) renderItem(i, row);
      return;
    }
  }
}

}  // namespace lcdtap
