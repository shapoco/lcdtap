#include <lcdtap/font8x16.hpp>
#include <lcdtap/osd.hpp>

#include <cstdio>
#include <cstring>

namespace lcdtap {

//=============================================================================
// getDefaultOsdConfig
//=============================================================================

void getDefaultOsdConfig(OsdConfig* cfg) {
  cfg->onMenuOpen = nullptr;
  cfg->onActionActivated = nullptr;
  cfg->userData = nullptr;
}

static LCDTAP_INLINE char normalizeChar(char c) {
  if (font8x16::CODE_FIRST <= c && c <= font8x16::CODE_LAST) {
    return c - font8x16::CODE_FIRST;
  } else {
    return ' ' - font8x16::CODE_FIRST;
  }
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
  lastInput_ = 0xFFu;  // prevent first-frame key repeat
  nextRepeatMs_ = 0;
  lastBlinkMs_ = 0;
  dumpScrollOffset_ = 0;
  lastDumpState_ = DumpState::WAIT;
  lastDumpSize_ = 0;
  dumpViewDirty_ = true;
  memset(textBuf_, normalizeChar(' '), sizeof(textBuf_));
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
    repeatPeriodMs_ = KEY_REPEAT_PERIOD_INIT_MS;
    nextRepeatMs_ = nowMs + KEY_REPEAT_DELAY_MS;
  } else if (input && nowMs >= nextRepeatMs_) {
    activeKeys = input;
    repeatPeriodMs_ = repeatPeriodMs_ * 31 / 32;
    if (repeatPeriodMs_ < KEY_REPEAT_PERIOD_MIN_MS) {
      repeatPeriodMs_ = KEY_REPEAT_PERIOD_MIN_MS;
    }
    nextRepeatMs_ = nowMs + repeatPeriodMs_;
  }
  lastInput_ = input;

  switch (state_) {
    case OsdState::HIDDEN: return updateHidden(lcdtap, nowMs, activeKeys);
    case OsdState::PRESET: return updatePresetList(lcdtap, nowMs, activeKeys);
    case OsdState::MAIN_MENU: return updateMainMenu(lcdtap, nowMs, activeKeys);
    case OsdState::DUMP_VIEW: return updateDumpView(lcdtap, nowMs, activeKeys);
    default: return OSD_ACTION_NONE;
  }
}

uint8_t Osd::updateHidden(LcdTap& lcdtap, uint64_t nowMs, uint8_t activeKeys) {
  uint8_t action = OSD_ACTION_NONE;
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
  return action;
}

uint8_t Osd::updateMainMenu(LcdTap& lcdtap, uint64_t nowMs,
                            uint8_t activeKeys) {
  uint8_t action = OSD_ACTION_NONE;

  // Navigate up/down
  if (activeKeys & OSD_KEY_UP) {
    // Skip disabled items when navigating
    do {
      selectedItem_ =
          (selectedItem_ == 0) ? (numItems_ - 1) : (selectedItem_ - 1);
    } while (!items_[selectedItem_].isEnabled);
    updateScroll();
  }
  if (activeKeys & OSD_KEY_DOWN) {
    // Skip disabled items when navigating
    do {
      selectedItem_ =
          (selectedItem_ == numItems_ - 1) ? 0 : (selectedItem_ + 1);
    } while (!items_[selectedItem_].isEnabled);
    updateScroll();
  }

  OsdMenuItem& sel = items_[selectedItem_];
  ConfigEntry& cfg = sel.config;

  if (sel.isAction) {
    if (activeKeys & OSD_KEY_ENTER) {
      if (cfg.value == OSD_ACTION_PRESET) {
        state_ = OsdState::PRESET;
        renderPresetList();
      } else if (sel.id == OSD_ITEM_ID_VIEW_DUMP) {
        state_ = OsdState::DUMP_VIEW;
        dumpScrollOffset_ = 0;
        lastDumpState_ = lcdtap.dumpGetState();
        lastDumpSize_ = lcdtap.dumpGetSize();
        renderDumpView(lcdtap);
      } else {
        bool stayOpen = false;
        if (cfg_.onActionActivated) {
          stayOpen = cfg_.onActionActivated(this, &sel, lcdtap, cfg_.userData);
        }
        if (!stayOpen) {
          action = static_cast<uint8_t>(cfg.value);
          if (action == OSD_ACTION_APPLY) {
            applyConfig(lcdtap);
          }
          state_ = OsdState::HIDDEN;
        }
      }
    }
  } else {
    // Adjust value with left/right
    bool changed = false;
    if (activeKeys & OSD_KEY_LEFT) {
      changed = true;
      switch (cfg.type) {
        case ValueType::INT16:
          if (cfg.value - cfg.step >= cfg.min)
            cfg.value = static_cast<int16_t>(cfg.value - cfg.step);
          else
            cfg.value = cfg.min;

          break;
        case ValueType::BOOL: cfg.value = (~cfg.value) & 1; break;
        case ValueType::ENUM:
          cfg.value = static_cast<int16_t>(cfg.value - cfg.step);
          if (cfg.value < cfg.min) cfg.value = cfg.max;
          break;
        default: break;
      }
    }
    if (activeKeys & OSD_KEY_RIGHT) {
      changed = true;
      switch (cfg.type) {
        case ValueType::INT16:
          if (cfg.value + cfg.step <= cfg.max)
            cfg.value = static_cast<int16_t>(cfg.value + cfg.step);
          else
            cfg.value = cfg.max;
          break;
        case ValueType::BOOL: cfg.value = (~cfg.value) & 1; break;
        case ValueType::ENUM:
          cfg.value = static_cast<int16_t>(cfg.value + cfg.step);
          if (cfg.value > cfg.max) cfg.value = cfg.min;
          break;
        default: break;
      }
    }
    if (changed) {
      updateItemEnables();
    }
    if (activeKeys & OSD_KEY_ENTER) {
      setSelectedIndex(getItemIndexById(OSD_ITEM_ID_APPLY));
      renderAll();
    }
  }

  if (state_ == OsdState::MAIN_MENU) renderAll();

  return action;
}

uint8_t Osd::updatePresetList(LcdTap& lcdtap, uint64_t nowMs,
                              uint8_t activeKeys) {
  int numPresets = static_cast<int>(ConfigPreset::NUM_PRESETS);
  if (activeKeys & OSD_KEY_LEFT) {
    state_ = OsdState::MAIN_MENU;
    renderAll();
  }
  if (activeKeys & OSD_KEY_ENTER) {
    // Load the selected preset
    LcdTapConfig cfg;
    getPresetConfig(static_cast<ConfigPreset>(presetSelectedIndex_), &cfg);
    loadConfig(cfg);
    state_ = OsdState::MAIN_MENU;
    setSelectedIndex(getItemIndexById(OSD_ITEM_ID_APPLY));
    renderAll();
  }
  if (activeKeys & OSD_KEY_UP) {
    presetSelectedIndex_ = (presetSelectedIndex_ + numPresets - 1) % numPresets;
    renderPresetList();
  }
  if (activeKeys & OSD_KEY_DOWN) {
    presetSelectedIndex_ = (presetSelectedIndex_ + 1) % numPresets;
    renderPresetList();
  }
  return OSD_ACTION_NONE;
}

uint8_t Osd::updateDumpView(LcdTap& lcdtap, uint64_t nowMs,
                            uint8_t activeKeys) {
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
  return OSD_ACTION_NONE;
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
    uint8_t bits =
        font8x16::bitmap[static_cast<uint32_t>(ch) * font8x16::GLYPH_HEIGHT +
                         static_cast<uint32_t>(pixRow)];
    uint8_t colByte = colPtr[col];
    uint16_t fg = OSD_PALETTE[colByte >> 4];
    uint16_t bg = OSD_PALETTE[colByte & 0xFu];
    *dst++ = (bits & 0x01) ? fg : bg;
    *dst++ = (bits & 0x02) ? fg : bg;
    *dst++ = (bits & 0x04) ? fg : bg;
    *dst++ = (bits & 0x08) ? fg : bg;
    *dst++ = (bits & 0x10) ? fg : bg;
    *dst++ = (bits & 0x20) ? fg : bg;
    *dst++ = (bits & 0x40) ? fg : bg;
    *dst++ = (bits & 0x80) ? fg : bg;
  }
}

void Osd::makeItemById(uint16_t id, OsdMenuItem* item) {
  memset(item, 0, sizeof(*item));
  item->id = id;

  switch (id) {
    case OSD_ITEM_ID_PRESET:
      item->isAction = true;
      item->config.name = "Load Preset";
      item->config.value = OSD_ACTION_PRESET;
      break;

    // Command Dump
    case OSD_ITEM_ID_VIEW_DUMP:
      item->isAction = true;
      item->config.name = "Command Dump";
      item->config.value = OSD_ACTION_NONE;
      break;

    // Apply
    case OSD_ITEM_ID_APPLY:
      item->isAction = true;
      item->config.name = "Apply";
      item->config.value = OSD_ACTION_APPLY;
      break;

    // Cancel
    case OSD_ITEM_ID_CANCEL:
      item->isAction = true;
      item->config.name = "Cancel";
      item->config.value = OSD_ACTION_CANCEL;
      break;

    default:
      uint16_t numConfigs = static_cast<uint16_t>(ConfigId::NUM_CONFIGS);
      if (OSD_ITEM_ID_SYS_BASE <= id &&
          id < OSD_USER_ITEM_ID_BASE + numConfigs) {
        getConfigEntryById(static_cast<ConfigId>(id - OSD_ITEM_ID_SYS_BASE),
                           &item->config);
      }
      break;
  }
}

//=============================================================================
// Osd::initMenuItems
//=============================================================================

void Osd::initMenuItems(const LcdTap& lcdtap) {
  LcdTapConfig cfg = lcdtap.getConfig();
  numItems_ = 0;

  for (uint16_t id = 1; id < OSD_NUM_SYSTEM_ITEMS; ++id) {
    OsdMenuItem& it = items_[numItems_++];
    makeItemById(id, &it);
    if (!it.isAction) {
      it.config.value = getConfigValueById(
          cfg, static_cast<ConfigId>(id - OSD_ITEM_ID_SYS_BASE));
      if (it.config.enableKeyId >= 0) {
        it.config.enableKeyId += OSD_ITEM_ID_SYS_BASE;
      }
    }
  }

  updateItemEnables();

  selectedItem_ = 0;
  scrollOffset_ = 0;
  renderAll();
}

void Osd::updateItemEnables() {
  for (int i = 0; i < numItems_; ++i) {
    OsdMenuItem& item = items_[i];
    bool isEnabled = true;
    if (item.config.enableKeyId >= 0) {
      const OsdMenuItem* enableKeyItem = nullptr;
      getItemById(static_cast<uint16_t>(item.config.enableKeyId),
                  &enableKeyItem);
      if (enableKeyItem) {
        const bool enableKeyIsEnabled = enableKeyItem->isEnabled;
        const int16_t enableKeyValue = enableKeyItem->config.value;
        if (!enableKeyIsEnabled ||
            enableKeyValue < item.config.enableKeyValueMin ||
            enableKeyValue > item.config.enableKeyValueMax) {
          isEnabled = false;
        }
      }
    }
    item.isEnabled = isEnabled;
  }
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

  uint8_t col = static_cast<uint8_t>((PAL_WHITE << 4) | PAL_BLACK);
  if (isSel) {
    col = static_cast<uint8_t>((PAL_WHITE << 4) | PAL_SKY_BLUE);
  } else if (!item.isEnabled) {
    col = static_cast<uint8_t>((PAL_DARK_GRAY << 4) | PAL_BLACK);
  }

  fillRowColor(row, col);

  // Name field
  writeStr(row, COL_NAME_START, item.config.name,
           COL_NAME_END - COL_NAME_START + 1);

  // Separator
  writeChar(row, COL_SEP, ':');

  if (item.isAction) {
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
    formatConfigValue(valBuf, static_cast<int>(sizeof(valBuf)), item.config);
    writeStr(row, COL_VAL_START, valBuf, COL_VAL_END - COL_VAL_START + 1);

    // Unit
    if (item.config.unit && item.config.unit[0]) {
      writeStr(row, COL_UNIT_START, item.config.unit,
               COL_UNIT_END - COL_UNIT_START + 1);
    }
  }
}

//=============================================================================
// Osd::applyConfig / buildConfig
//=============================================================================

LcdTapConfig Osd::buildConfig() const {
  const OsdMenuItem* ctrlItem;
  getItemById(
      OSD_ITEM_ID_SYS_BASE + static_cast<uint16_t>(ConfigId::CTRL_FAMILY),
      &ctrlItem);
  ControllerFamily controller =
      static_cast<ControllerFamily>(ctrlItem ? ctrlItem->config.value : 0);

  LcdTapConfig cfg;
  getDefaultConfig(controller, &cfg);

  uint16_t numConfigs = static_cast<uint16_t>(lcdtap::ConfigId::NUM_CONFIGS);
  for (uint16_t cfgId = 0; cfgId < numConfigs; ++cfgId) {
    const OsdMenuItem* item = nullptr;
    getItemById(OSD_ITEM_ID_SYS_BASE + cfgId, &item);
    if (item) {
      setConfigValueById(&cfg, static_cast<ConfigId>(cfgId),
                         item->config.value);
    }
  }

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
  memset(&textBuf_[row * COLS], normalizeChar(c), static_cast<size_t>(COLS));
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
    dst[i] = normalizeChar(str[i]);
    ++i;
  }
}

void Osd::writeChar(int row, int col, char c) {
  textBuf_[row * COLS + col] = normalizeChar(c);
}

//=============================================================================
// Osd::renderPresetList
//=============================================================================
void Osd::renderPresetList() {
  constexpr int HEADER_ROWS = 2;
  constexpr int FOOTER_ROWS = 1;
  constexpr int BODY_ROWS = ROWS - HEADER_ROWS - FOOTER_ROWS;

  // Ensure selected index is visible within the scroll window
  int p = presetSelectedIndex_ - presetScrollOffset_;
  if (p < 0) {
    presetScrollOffset_ = presetSelectedIndex_;
  } else if (p >= BODY_ROWS) {
    presetScrollOffset_ = presetSelectedIndex_ - BODY_ROWS + 1;
  }

  // Clear background to avoid main-menu color bleed-through
  memset(textCol_, static_cast<uint8_t>((PAL_WHITE << 4) | PAL_BLACK),
         sizeof(textCol_));

  drawTitleBar("Load Preset");

  // Header
  fillRow(1, ' ');
  fillRowColor(1, static_cast<uint8_t>((PAL_SILVER << 4) | PAL_DARK_GRAY));
  writeStr(1, 0, "Preset Name      Family   Bus   Size");

  // Body
  int numPresets = static_cast<int>(ConfigPreset::NUM_PRESETS);
  for (int slot = 0; slot < BODY_ROWS; ++slot) {
    char buff[COLS + 1];
    LcdTapConfig preset;
    int i = presetScrollOffset_ + slot;
    uint8_t col = static_cast<uint8_t>((PAL_WHITE << 4) | PAL_BLACK);
    if (i == presetSelectedIndex_) {
      col = static_cast<uint8_t>((PAL_WHITE << 4) | PAL_SKY_BLUE);
    }
    fillRow(slot + HEADER_ROWS, ' ');
    fillRowColor(slot + HEADER_ROWS, col);
    if (0 <= i && i < numPresets) {
      getPresetConfig(static_cast<ConfigPreset>(i), &preset);
      int row = HEADER_ROWS + slot;
      int iCtrl = static_cast<int>(preset.controllerFamily);
      snprintf(buff, sizeof(buff), "%-16s %-8s %-5s %dx%d",
               CONFIG_PRESET_NAMES[i], CONTROLLER_NAMES[iCtrl],
               BUS_SHORT_NAMES[static_cast<int>(preset.busInterface)],
               preset.buffWidth, preset.buffHeight);
      writeStr(row, 0, buff, COLS);
    }
  }

  // Footer
  fillRow(ROWS - 1, ' ');
  fillRowColor(ROWS - 1,
               static_cast<uint8_t>((PAL_SILVER << 4) | PAL_DARK_GRAY));
  writeStr(ROWS - 1, 0, "\x84:Back \x86:Load \x82\x83:Scroll");
}

//=============================================================================
// Osd::renderDumpView
//=============================================================================

void Osd::renderDumpView(LcdTap& lcdtap) {
  constexpr int HEADER_ROWS = 2;
  constexpr int FOOTER_ROWS = 1;
  constexpr int BODY_ROWS = ROWS - HEADER_ROWS - FOOTER_ROWS;
  constexpr int ENTRIES_PER_ROW = 16;

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

  // Header
  fillRow(1, ' ');
  fillRowColor(1, static_cast<uint8_t>((PAL_SILVER << 4) | PAL_DARK_GRAY));
  writeStr(1, 0, "   +0+1+2+3+4+5+6+7+8+9+A+B+C+D+E+F");

  // --- Rows 3-14: data rows ---
  for (int slot = 0; slot < BODY_ROWS; ++slot) {
    const int row = slot + HEADER_ROWS;
    const int rowIdx = dumpScrollOffset_ + slot;
    const int baseEntry = rowIdx * ENTRIES_PER_ROW;

    fillRow(row, ' ');

    // Address (e.g. "A0")
    writeChar(row, 0, HEX[rowIdx]);
    writeChar(row, 1, '0');
    setColorRange(row, 0, 3,
                  static_cast<uint8_t>((PAL_SILVER << 4) | PAL_DARK_GRAY));

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

  // Footer
  fillRow(ROWS - 1, ' ');
  fillRowColor(ROWS - 1,
               static_cast<uint8_t>((PAL_SILVER << 4) | PAL_DARK_GRAY));
  writeStr(ROWS - 1, 0, "\x84:Back \x86:Trigger \x85:Abort \x82\x83:Scroll");
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
  uint16_t numConfigs = static_cast<uint16_t>(lcdtap::ConfigId::NUM_CONFIGS);
  for (uint16_t cfgId = 0; cfgId < numConfigs; ++cfgId) {
    const OsdMenuItem* item = nullptr;
    getItemById(OSD_ITEM_ID_SYS_BASE + cfgId, &item);
    if (item) {
      setItemValue(item->id,
                   getConfigValueById(cfg, static_cast<ConfigId>(cfgId)));
    }
  }
  updateItemEnables();
  renderAll();
}

void Osd::setItemValue(uint16_t id, int16_t value) {
  for (int i = 0; i < numItems_; ++i) {
    if (items_[i].id == id) {
      items_[i].config.value = value;
      int row = i - scrollOffset_ + 1;
      if (row >= 1 && row < ROWS) renderItem(i, row);
      return;
    }
  }
}

}  // namespace lcdtap
