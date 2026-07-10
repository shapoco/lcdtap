#pragma once

// LcdTap OSD — On-Screen Display overlay for runtime configuration.
//
// Usage:
//   #include <lcdtap/osd.hpp>

#include <cstdint>

#include "lcdtap.hpp"

namespace lcdtap {

//=============================================================================
// OSD raster dimensions
//=============================================================================
static constexpr int OSD_WIDTH = 320;
static constexpr int OSD_HEIGHT = 240;

//=============================================================================
// Key codes (bit-field, combine with |)
//=============================================================================
static constexpr uint8_t OSD_KEY_UP = (1u << 0);
static constexpr uint8_t OSD_KEY_DOWN = (1u << 1);
static constexpr uint8_t OSD_KEY_LEFT = (1u << 2);
static constexpr uint8_t OSD_KEY_RIGHT = (1u << 3);
static constexpr uint8_t OSD_KEY_ENTER = (1u << 4);

//=============================================================================
// Action codes returned by Osd::update()
//=============================================================================
static constexpr uint8_t OSD_ACTION_NONE = 0;
static constexpr uint8_t OSD_ACTION_PRESET = 1;
static constexpr uint8_t OSD_ACTION_CANCEL = 2;
static constexpr uint8_t OSD_ACTION_APPLY = 3;

// Internal item IDs (< OSD_USER_ITEM_ID_BASE)
static constexpr uint16_t OSD_ITEM_ID_NONE = 0;
static constexpr uint16_t OSD_ITEM_ID_PRESET = 1;
static constexpr uint16_t OSD_ITEM_ID_SYS_BASE = OSD_ITEM_ID_PRESET + 1;
static constexpr uint16_t OSD_ITEM_ID_VIEW_DUMP =
    OSD_ITEM_ID_SYS_BASE + static_cast<uint16_t>(ConfigId::NUM_CONFIGS);
static constexpr uint16_t OSD_ITEM_ID_APPLY = OSD_ITEM_ID_VIEW_DUMP + 1;
static constexpr uint16_t OSD_ITEM_ID_CANCEL = OSD_ITEM_ID_APPLY + 1;
static constexpr uint16_t OSD_NUM_SYSTEM_ITEMS = OSD_ITEM_ID_CANCEL + 1;

// User-defined item IDs must be >= this value. IDs below are reserved for
// built-in items managed internally by the Osd class.
static constexpr uint16_t OSD_USER_ITEM_ID_BASE = 0x8000u;

//=============================================================================
// Menu item descriptor
//=============================================================================
struct OsdMenuItem {
  uint16_t id;  // Unique identifier; 0 = unassigned
  bool isAction;
  bool isEnabled;
  ConfigEntry config;
};

//=============================================================================
// OSD state
//=============================================================================
enum class OsdState : uint8_t {
  HIDDEN,     // OSD not visible
  MAIN_MENU,  // configuration menu
  PRESET,     // reset confirmation prompt
  DUMP_VIEW,  // command dump viewer
};

//=============================================================================
// OSD configuration (provided by host before calling Osd::init())
//=============================================================================
struct OsdConfig {
  // Called after initMenuItems(); use osd->insertItem() to inject custom items.
  void (*onMenuOpen)(class Osd* osd, void* userData);

  // Called when Enter is pressed on an ACTION menu item.
  // Return true to keep the OSD open (host handled the action).
  // Return false for default behaviour (Apply → applyConfig, then close OSD).
  // Set to nullptr to use default behaviour for all ACTION items.
  bool (*onActionActivated)(class Osd* osd, const OsdMenuItem* item,
                            LcdTap& lcdtap, void* userData);

  void* userData;
};

// Fill cfg with safe defaults.
void getDefaultOsdConfig(OsdConfig* cfg);

//=============================================================================
// Osd class
//=============================================================================
class Osd {
 public:
  // Store cfg and reset internal state.
  void init(const OsdConfig& cfg);

  // Call once per frame.
  // nowMs  : current time in milliseconds (used for key-repeat and blink).
  // lcdtap : LcdTap instance to read/write config from.
  // input  : bitmask of currently pressed keys (OSD_KEY_XXX).
  // Returns OSD_ACTION_APPLY or OSD_ACTION_CANCEL when the OSD closes,
  // OSD_ACTION_NONE otherwise.
  uint8_t update(uint64_t nowMs, LcdTap& lcdtap, uint8_t input);

  // Overlay the OSD onto an already-rendered DVI scanline.
  // No-op while the OSD is hidden.
  // line : DVI line number (0-based).
  // dst  : buffer of dviWidth uint16_t values (modified in-place for OSD rows).
  void fillScanline(uint16_t line, uint16_t* dst) const;

  // Current display state (e.g. to skip compositing while hidden).
  OsdState getState() const { return state_; }

  int getItemCount() const;
  int getSelectedIndex() const;
  void setSelectedIndex(int index);
  int getItemIndexById(uint16_t id) const;
  void getItemByIndex(int index, const OsdMenuItem** item) const;
  void getItemById(uint16_t id, const OsdMenuItem** item) const;
  void insertItem(int index, const OsdMenuItem& item);

  // Populate all built-in menu item values from cfg.
  // Call from onActionActivated to load a preset without closing the OSD.
  void loadConfig(const LcdTapConfig& cfg);

  // Update the value field of the item with the given id and re-render it.
  void setItemValue(uint16_t id, int16_t value);

 private:
  // OSD raster dimensions
  static constexpr int COLS = 40;
  static constexpr int ROWS = 15;
  static constexpr int MAX_ITEMS = 32;

  // Key repeat timing (ms)
  static constexpr uint64_t KEY_REPEAT_DELAY_MS = 500;
  static constexpr uint64_t KEY_REPEAT_PERIOD_INIT_MS = 200;
  static constexpr uint64_t KEY_REPEAT_PERIOD_MIN_MS = 50;

  // Blink timing (ms; half-period → toggles at 1 Hz)
  static constexpr uint64_t BLINK_PERIOD_MS = 500;

  // Color palette (RGB565); index = palette entry
  // clang-format off
  static constexpr uint16_t OSD_PALETTE[16] = {
      0x0000u, 0x4208u, 0x8410u, 0xC618u,  // 0:BLACK 1:DARK_GRAY 2:GRAY 3:SILVER
      0xFFFFu, 0xF808u, 0xFF00u, 0x07E0u,  // 4:WHITE 5:RED 6:YELLOW 7:GREEN
      0x061Fu, 0x021Fu, 0xF81Fu, 0x0000u,  // 8:CYAN 9:BLUE 10:MAGENTA 11:(rsv)
      0x0000u, 0x0000u, 0x000Fu, 0x0318u,  // 12:(rsv) 13:(rsv) 14:TITLE_BG 15:SEL_BG
  };
  // clang-format on
  static constexpr uint8_t PAL_BLACK = 0;
  static constexpr uint8_t PAL_DARK_GRAY = 1;
  static constexpr uint8_t PAL_GRAY = 2;
  static constexpr uint8_t PAL_SILVER = 3;
  static constexpr uint8_t PAL_WHITE = 4;
  static constexpr uint8_t PAL_RED = 5;
  static constexpr uint8_t PAL_YELLOW = 6;
  static constexpr uint8_t PAL_CYAN = 8;
  static constexpr uint8_t PAL_BLUE = 9;
  static constexpr uint8_t PAL_DARK_BLUE = 14;
  static constexpr uint8_t PAL_SKY_BLUE = 15;

  // Text line layout column positions (main menu)
  static constexpr int COL_NAME_START = 0;
  static constexpr int COL_NAME_END = 17;  // inclusive, 18 chars
  static constexpr int COL_SEP = 18;       // ':'
  static constexpr int COL_IND_LEFT = 21;  // '◀' (0x80) or ' '
  static constexpr int COL_VAL_START = 23;
  static constexpr int COL_VAL_END = 33;    // inclusive, 11 chars
  static constexpr int COL_IND_RIGHT = 35;  // '▶' (0x81) or ' '
  static constexpr int COL_UNIT_START = 37;
  static constexpr int COL_UNIT_END = 39;  // inclusive, 3 chars

  char textBuf_[COLS * ROWS];  // flat character raster, row-major
  uint8_t
      textCol_[COLS * ROWS];  // per-char color: upper 4 bit=fg, lower 4 bit=bg
  OsdMenuItem items_[MAX_ITEMS];
  int numItems_;
  int selectedItem_;
  int scrollOffset_;  // index of the first visible item
  OsdState state_;
  bool blinkOn_;

  uint8_t lastInput_;
  uint64_t nextRepeatMs_;
  uint32_t repeatPeriodMs_;
  uint64_t lastBlinkMs_;

  OsdConfig cfg_;

  // Preset list state
  int presetScrollOffset_ = 0;
  int presetSelectedIndex_ = 0;

  // Dump view state
  int dumpScrollOffset_ = 0;
  DumpState lastDumpState_ = DumpState::WAIT;
  uint16_t lastDumpSize_ = 0;
  bool dumpViewDirty_ = true;

  static void makeItemById(uint16_t id, OsdMenuItem* item);

  // Populate items_[] from the current LcdTap config.
  void initMenuItems(const LcdTap& lcdtap);

  // Update the isEnabled field of all items based on their enableKeyId.
  void updateItemEnables();

  uint8_t updateHidden(LcdTap& lcdtap, uint64_t nowMs, uint8_t activeKeys);
  uint8_t updateMainMenu(LcdTap& lcdtap, uint64_t nowMs, uint8_t activeKeys);
  uint8_t updatePresetList(LcdTap& lcdtap, uint64_t nowMs, uint8_t activeKeys);
  uint8_t updateDumpView(LcdTap& lcdtap, uint64_t nowMs, uint8_t activeKeys);

  // Rebuild the entire text buffer from items_ and display state.
  void renderAll();
  void renderTitle();
  void renderItem(int idx, int row);

  // Render the preset selection menu into textBuf_/textCol_.
  void renderPresetList();

  // Render the dump viewer into textBuf_/textCol_.
  void renderDumpView(LcdTap& lcdtap);

  // Adjust scrollOffset_ so that selectedItem_ is visible.
  void updateScroll();

  // Apply the pending config stored in items_[] to lcdtap.
  void applyConfig(LcdTap& lcdtap) const;

  // Build a LcdTapConfig from the current item values.
  LcdTapConfig buildConfig() const;

  // --- Text helpers ---

  // Draw a horizontal bar across the entire row using the given character.
  void drawTitleBar(const char* title);

  // Fill an entire row with the given character.
  void fillRow(int row, char c);

  // Fill an entire row's color buffer with the given byte.
  void fillRowColor(int row, uint8_t colByte);

  // Fill a range of columns in the color buffer.
  void setColorRange(int row, int col, int len, uint8_t colByte,
                     uint8_t mask = 0xFF);

  // Write a null-terminated string into the text buffer.
  // maxLen < 0 means "no limit" (caller must ensure it fits within COLS).
  void writeStr(int row, int col, const char* str, int maxLen = -1);

  // Write a single character.
  void writeChar(int row, int col, char c);
};

}  // namespace lcdtap
