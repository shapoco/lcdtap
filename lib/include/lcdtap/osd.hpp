#pragma once

// LcdTap OSD — On-Screen Display overlay for runtime configuration.
//
// Usage:
//   #include <lcdtap/osd.hpp>

#include <cstdint>

#include "lcdtap.hpp"

namespace lcdtap {

//=============================================================================
// Key codes (bit-field, combine with |)
//=============================================================================
static const uint8_t OSD_KEY_UP = (1u << 0);
static const uint8_t OSD_KEY_DOWN = (1u << 1);
static const uint8_t OSD_KEY_LEFT = (1u << 2);
static const uint8_t OSD_KEY_RIGHT = (1u << 3);
static const uint8_t OSD_KEY_ENTER = (1u << 4);

//=============================================================================
// Action codes returned by Osd::update()
//=============================================================================
static const uint8_t OSD_ACTION_NONE = 0;
static const uint8_t OSD_ACTION_CANCEL = 1;
static const uint8_t OSD_ACTION_APPLY = 2;

// User-defined item IDs must be >= this value. IDs below are reserved for
// built-in items managed internally by the Osd class.
static const uint16_t OSD_USER_ITEM_ID_BASE = 0x8000u;

//=============================================================================
// Menu item type
//=============================================================================
enum class OsdMenuType : uint8_t {
  ACTION,   // Triggers an action on Enter
  INTEGER,  // Numeric value (clamped at min/max)
  BOOL,     // Boolean: left=OFF, right=ON
  ENUM,     // String selection (wraps around)
};

//=============================================================================
// Menu item descriptor
//=============================================================================
struct OsdMenuItem {
  uint16_t id;  // Unique identifier; 0 = unassigned
  OsdMenuType type;
  const char* name;     // Item label
  const char* unit;     // Unit string shown after the value (e.g. "px", "deg")
  const char** values;  // Display strings for ENUM type (nullptr otherwise)
  int16_t min;          // Minimum value (INTEGER / ENUM index)
  int16_t max;          // Maximum value (INTEGER / ENUM index)
  int16_t step;         // Increment per key press (INTEGER / ENUM)
  int16_t value;        // Current value; for ACTION: OSD_ACTION_XXX
};

//=============================================================================
// OSD configuration (provided by host before calling Osd::init())
//=============================================================================
struct OsdConfig {
  // Reserved for future host-side extensions.
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

  int getItemCount() const;
  int getSelectedIndex() const;
  void setSelectedIndex(int index);
  void getItemByIndex(int index, const OsdMenuItem** item) const;
  void getItemById(uint16_t id, const OsdMenuItem** item) const;
  void insertItem(int index, const OsdMenuItem& item);

 private:
  // OSD raster dimensions
  static constexpr int COLS = 40;
  static constexpr int ROWS = 15;
  static constexpr int OSD_WIDTH = 320;   // COLS * GLYPH_WIDTH  (40*8)
  static constexpr int OSD_HEIGHT = 240;  // ROWS * GLYPH_HEIGHT (15*16)
  static constexpr int MAX_ITEMS = 32;

  // Key repeat timing (ms)
  static constexpr uint64_t KEY_REPEAT_DELAY_MS = 500;
  static constexpr uint64_t KEY_REPEAT_PERIOD_MS = 200;

  // Blink timing (ms; half-period → toggles at 1 Hz)
  static constexpr uint64_t BLINK_PERIOD_MS = 500;

  // OSD color palette (RGB565)
  static constexpr uint16_t COLOR_BG = 0x0000u;        // black
  static constexpr uint16_t COLOR_FG = 0xFFFFu;        // white
  static constexpr uint16_t COLOR_TITLE_BG = 0x000Fu;  // dark navy
  static constexpr uint16_t COLOR_TITLE_FG = 0xFFFFu;  // white
  static constexpr uint16_t COLOR_SEL_BG = 0x0318u;    // dark teal
  static constexpr uint16_t COLOR_SEL_FG = 0xFFFFu;    // white

  // Text line layout column positions
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
  OsdMenuItem items_[MAX_ITEMS];
  int numItems_;
  int selectedItem_;
  int scrollOffset_;  // index of the first visible item
  bool visible_;
  bool blinkOn_;

  uint8_t lastInput_;
  uint64_t keyPressMs_;
  uint64_t lastRepeatMs_;
  uint64_t lastBlinkMs_;

  OsdConfig cfg_;

  // Populate items_[] from the current LcdTap config.
  void initMenuItems(const LcdTap& lcdtap);

  // Rebuild the entire text buffer from items_ and display state.
  void renderAll();
  void renderTitle();
  void renderItem(int idx, int row);

  // Adjust scrollOffset_ so that selectedItem_ is visible.
  void updateScroll();

  // Apply the pending config stored in items_[] to lcdtap.
  void applyConfig(LcdTap& lcdtap) const;

  // Build a LcdTapConfig from the current item values.
  LcdTapConfig buildConfig() const;

  // --- Text helpers ---

  // Fill an entire row with the given character.
  void fillRow(int row, char c);

  // Write a null-terminated string into the text buffer.
  // maxLen < 0 means "no limit" (caller must ensure it fits within COLS).
  void writeStr(int row, int col, const char* str, int maxLen = -1);

  // Write a single character.
  void writeChar(int row, int col, char c);

  // Format the current value of item as a string into buf.
  void formatValue(char* buf, int bufLen, const OsdMenuItem& item) const;
};

}  // namespace lcdtap
