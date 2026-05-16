#include <lcdtap/font8x16.hpp>
#include <lcdtap/osd.hpp>

#include <cstdio>
#include <cstring>

namespace lcdtap {

//=============================================================================
// File-scope lookup tables
//=============================================================================

static const char* kControllerNames[] = {"ST7789", "SSD1306"};
static constexpr int kNumControllers = 2;

static const PixelFormat kPixelFormatMap[] = {
    PixelFormat::MONO_VPACK,
    PixelFormat::RGB444,
    PixelFormat::RGB565,
    PixelFormat::RGB666,
};
static const char* kPixelFormatNames[] = {"MONO", "RGB444", "RGB565", "RGB666"};
static constexpr int kNumPixelFormats = 4;

static const char* kOnOffNames[] = {"OFF", "ON"};

static const char* kRotationNames[] = {"0", "90", "180", "270"};

static const char* kScaleModeNames[] = {"STRETCH", "FIT", "PIXEL_PERF"};
static constexpr int kNumScaleModes = 3;

// Internal item IDs (< OSD_USER_ITEM_ID_BASE)
namespace {
constexpr uint16_t ITEM_ID_CONTROLLER = 1u;
constexpr uint16_t ITEM_ID_PIXEL_FMT = 2u;
constexpr uint16_t ITEM_ID_LCD_WIDTH = 3u;
constexpr uint16_t ITEM_ID_LCD_HEIGHT = 4u;
constexpr uint16_t ITEM_ID_INVERSION = 5u;
constexpr uint16_t ITEM_ID_SWAP_RB = 6u;
constexpr uint16_t ITEM_ID_OUTPUT_ROT = 7u;
constexpr uint16_t ITEM_ID_SCALE_MODE = 8u;
constexpr uint16_t ITEM_ID_APPLY = 9u;
constexpr uint16_t ITEM_ID_CANCEL = 10u;
}  // namespace

//=============================================================================
// getDefaultOsdConfig
//=============================================================================

void getDefaultOsdConfig(OsdConfig* cfg) {
  cfg->onMenuOpen = nullptr;
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
  visible_ = false;
  blinkOn_ = true;
  lastInput_ = 0;
  keyPressMs_ = 0;
  lastRepeatMs_ = 0;
  lastBlinkMs_ = 0;
  memset(textBuf_, ' ', sizeof(textBuf_));
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

  if (!visible_) {
    // Show OSD on Enter
    if (activeKeys & OSD_KEY_ENTER) {
      initMenuItems(lcdtap);
      if (cfg_.onMenuOpen) cfg_.onMenuOpen(this, cfg_.userData);
      visible_ = true;
      blinkOn_ = true;
      lastBlinkMs_ = nowMs;
    }
  } else {
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
        action = static_cast<uint8_t>(sel.value);
        if (action == OSD_ACTION_APPLY) {
          applyConfig(lcdtap);
        }
        visible_ = false;
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
          case OsdMenuType::BOOL: sel.value = 0; break;
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
          case OsdMenuType::BOOL: sel.value = 1; break;
          case OsdMenuType::ENUM:
            sel.value = static_cast<int16_t>(sel.value + sel.step);
            if (sel.value > sel.max) sel.value = sel.min;
            break;
          default: break;
        }
      }
    }

    renderAll();
  }

  return action;
}

//=============================================================================
// Osd::fillScanline
//=============================================================================

void Osd::fillScanline(uint16_t line, uint16_t* dst) const {
  if (!visible_ || line >= static_cast<uint16_t>(OSD_HEIGHT)) return;

  const int textRow = line >> 4;   // line / GLYPH_HEIGHT (16)
  const int pixRow = line & 0xFu;  // line % GLYPH_HEIGHT

  const bool isTitle = (textRow == 0);
  const bool isSel =
      !isTitle && (textRow == (selectedItem_ - scrollOffset_) + 1);

  const uint16_t bg =
      isTitle ? COLOR_TITLE_BG : (isSel ? COLOR_SEL_BG : COLOR_BG);
  const uint16_t fg =
      isTitle ? COLOR_TITLE_FG : (isSel ? COLOR_SEL_FG : COLOR_FG);

  const char* rowPtr = &textBuf_[textRow * COLS];
  const uint8_t codeFirst = static_cast<uint8_t>(font8x16::CODE_FIRST);
  const uint8_t codeLast = static_cast<uint8_t>(font8x16::CODE_LAST);

  for (int col = 0; col < COLS; ++col) {
    uint8_t ch = static_cast<uint8_t>(rowPtr[col]);
    if (ch < codeFirst || ch > codeLast) ch = static_cast<uint8_t>(' ');
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

  // Helper lambda to append an item and assign its ID
  auto add = [this](uint16_t id) -> OsdMenuItem& {
    OsdMenuItem& it = items_[numItems_++];
    it.id = id;
    return it;
  };

  // Controller Type
  {
    OsdMenuItem& it = add(ITEM_ID_CONTROLLER);
    it.type = OsdMenuType::ENUM;
    it.name = "Controller Type";
    it.unit = "";
    it.values = kControllerNames;
    it.min = 0;
    it.max = static_cast<int16_t>(kNumControllers - 1);
    it.step = 1;
    it.value = static_cast<int16_t>(cfg.controller);
  }

  // Pixel Format (non-contiguous enum → map to index)
  {
    int pfIdx = 0;
    for (int i = 0; i < kNumPixelFormats; ++i) {
      if (kPixelFormatMap[i] == cfg.pixelFormat) {
        pfIdx = i;
        break;
      }
    }
    OsdMenuItem& it = add(ITEM_ID_PIXEL_FMT);
    it.type = OsdMenuType::ENUM;
    it.name = "Pixel Format";
    it.unit = "";
    it.values = kPixelFormatNames;
    it.min = 0;
    it.max = static_cast<int16_t>(kNumPixelFormats - 1);
    it.step = 1;
    it.value = static_cast<int16_t>(pfIdx);
  }

  // LCD Width
  {
    OsdMenuItem& it = add(ITEM_ID_LCD_WIDTH);
    it.type = OsdMenuType::INTEGER;
    it.name = "LCD Width";
    it.unit = "px";
    it.values = nullptr;
    it.min = 32;
    it.max = 480;
    it.step = 8;
    it.value = static_cast<int16_t>(cfg.lcdWidth);
  }

  // LCD Height
  {
    OsdMenuItem& it = add(ITEM_ID_LCD_HEIGHT);
    it.type = OsdMenuType::INTEGER;
    it.name = "LCD Height";
    it.unit = "px";
    it.values = nullptr;
    it.min = 32;
    it.max = 480;
    it.step = 8;
    it.value = static_cast<int16_t>(cfg.lcdHeight);
  }

  // Inversion
  {
    OsdMenuItem& it = add(ITEM_ID_INVERSION);
    it.type = OsdMenuType::BOOL;
    it.name = "Inversion";
    it.unit = "";
    it.values = kOnOffNames;
    it.min = 0;
    it.max = 1;
    it.step = 1;
    it.value = cfg.invertInvPolarity ? 1 : 0;
  }

  // Swap R/B
  {
    OsdMenuItem& it = add(ITEM_ID_SWAP_RB);
    it.type = OsdMenuType::BOOL;
    it.name = "Swap R/B";
    it.unit = "";
    it.values = kOnOffNames;
    it.min = 0;
    it.max = 1;
    it.step = 1;
    it.value = cfg.swapRB ? 1 : 0;
  }

  // Output Rotation
  {
    OsdMenuItem& it = add(ITEM_ID_OUTPUT_ROT);
    it.type = OsdMenuType::ENUM;
    it.name = "Output Rotation";
    it.unit = "deg";
    it.values = kRotationNames;
    it.min = 0;
    it.max = 3;
    it.step = 1;
    it.value = static_cast<int16_t>(cfg.outputRotation & 3u);
  }

  // Scale Mode
  {
    OsdMenuItem& it = add(ITEM_ID_SCALE_MODE);
    it.type = OsdMenuType::ENUM;
    it.name = "Scale Mode";
    it.unit = "";
    it.values = kScaleModeNames;
    it.min = 0;
    it.max = static_cast<int16_t>(kNumScaleModes - 1);
    it.step = 1;
    it.value = static_cast<int16_t>(cfg.scaleMode);
  }

  // Apply
  {
    OsdMenuItem& it = add(ITEM_ID_APPLY);
    it.type = OsdMenuType::ACTION;
    it.name = "Apply";
    it.unit = "";
    it.values = nullptr;
    it.min = 0;
    it.max = 0;
    it.step = 0;
    it.value = OSD_ACTION_APPLY;
  }

  // Cancel
  {
    OsdMenuItem& it = add(ITEM_ID_CANCEL);
    it.type = OsdMenuType::ACTION;
    it.name = "Cancel";
    it.unit = "";
    it.values = nullptr;
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
    if (idx < numItems_)
      renderItem(idx, slot + 1);
    else
      fillRow(slot + 1, ' ');
  }
}

void Osd::renderTitle() {
  fillRow(0, ' ');
  writeStr(0, 0, "==== LcdTap Configuration ==============");
}

void Osd::renderItem(int idx, int row) {
  fillRow(row, ' ');

  const OsdMenuItem& item = items_[idx];
  const bool isSel = (idx == selectedItem_);

  // Name field
  writeStr(row, COL_NAME_START, item.name, COL_NAME_END - COL_NAME_START + 1);

  // Separator
  writeChar(row, COL_SEP, ':');

  if (item.type == OsdMenuType::ACTION) {
    // "HIT ENTER" centered in columns COL_IND_LEFT..COL_IND_RIGHT when
    // selected and blinking
    if (isSel && blinkOn_) {
      const char* label = "HIT ENTER";
      const int labelLen = 9;
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
  LcdTapConfig cfg = {};

  auto get = [this](uint16_t id, int16_t def) -> int16_t {
    const OsdMenuItem* it = nullptr;
    getItemById(id, &it);
    return it ? it->value : def;
  };

  cfg.controller = static_cast<ControllerType>(get(ITEM_ID_CONTROLLER, 0));

  const int pfIdx = get(ITEM_ID_PIXEL_FMT, 2);
  cfg.pixelFormat = (pfIdx >= 0 && pfIdx < kNumPixelFormats)
                        ? kPixelFormatMap[pfIdx]
                        : PixelFormat::RGB565;

  cfg.lcdWidth = static_cast<uint16_t>(get(ITEM_ID_LCD_WIDTH, 240));
  cfg.lcdHeight = static_cast<uint16_t>(get(ITEM_ID_LCD_HEIGHT, 320));
  cfg.invertInvPolarity = (get(ITEM_ID_INVERSION, 0) != 0);
  cfg.swapRB = (get(ITEM_ID_SWAP_RB, 0) != 0);
  // dviWidth / dviHeight: not set here; preserved by applyConfig()
  cfg.outputRotation = static_cast<uint8_t>(get(ITEM_ID_OUTPUT_ROT, 0) & 3u);
  cfg.scaleMode = static_cast<ScaleMode>(get(ITEM_ID_SCALE_MODE, 0));

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

void Osd::fillRow(int row, char c) {
  memset(&textBuf_[row * COLS], c, static_cast<size_t>(COLS));
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
      if (item.values && item.value >= item.min && item.value <= item.max) {
        snprintf(buf, static_cast<size_t>(bufLen), "%s",
                 item.values[item.value - item.min]);
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

void Osd::getItemByIndex(int index, const OsdMenuItem** item) const {
  *item = (index >= 0 && index < numItems_) ? &items_[index] : nullptr;
}

void Osd::getItemById(uint16_t id, const OsdMenuItem** item) const {
  for (int i = 0; i < numItems_; ++i) {
    if (items_[i].id == id) {
      *item = &items_[i];
      return;
    }
  }
  *item = nullptr;
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

}  // namespace lcdtap
