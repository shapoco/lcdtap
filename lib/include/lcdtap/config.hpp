#ifndef LCDTAP_CONFIG_HPP
#define LCDTAP_CONFIG_HPP

#include <cstdint>
#include <cstring>

namespace lcdtap {

//=============================================================================
// LCD controller families
//=============================================================================
enum class ControllerFamily : uint8_t {
  ST7789,
  SSD1306,
  SSD1331,
  ILI9341,
  ST7032,
  KS0108,
  NUM_CONTROLLERS,
};

static const char* CONTROLLER_NAMES[] = {"ST7789",  "SSD1306", "SSD1331",
                                         "ILI9341", "ST7032",  "KS0108"};
static_assert(sizeof(CONTROLLER_NAMES) / sizeof(CONTROLLER_NAMES[0]) ==
                  static_cast<size_t>(ControllerFamily::NUM_CONTROLLERS),
              "CONTROLLER_NAMES size must match ControllerFamily enum");

//=============================================================================
// Bus types (physical interface between host and LCD controller)
//=============================================================================

enum class BusType : uint8_t {
  I2C,
  SPI_4LINE,
  SPI_3LINE,
  PARALLEL,
  PARALLEL_2CS,
  NUM_BUSES,
};

static const char* BUS_NAMES[] = {"I2C", "4-Line SPI", "3-Line SPI",
                                  "Parallel8", "Para8 Dual CS"};
static_assert(sizeof(BUS_NAMES) / sizeof(BUS_NAMES[0]) ==
                  static_cast<size_t>(BusType::NUM_BUSES),
              "BUS_NAMES size must match lcdtap::BusType enum");

static const char* BUS_SHORT_NAMES[] = {"I2C", "SPI4", "SPI3", "PAR8", "P8C2"};
static_assert(sizeof(BUS_SHORT_NAMES) / sizeof(BUS_SHORT_NAMES[0]) ==
                  static_cast<size_t>(BusType::NUM_BUSES),
              "BUS_SHORT_NAMES size must match lcdtap::BusType enum");

//=============================================================================
// Interface Pixel format (SPI/I2C input side)
//=============================================================================
enum class InterfaceFormat : uint8_t {
  // 1bpp monochrome, vertical 8-pixel pack, high-to-low
  // e.g. SSD1306
  GRAY1_VPACK8_H2L,

  // 3bpp RGB, horizontal 2-pixel pack, high-to-low, 8bit right-aligned
  // e.g. ILI9488
  RGB111_HPACK2_H2L_RA8,

  // 8bpp RGB, 3-3-2 bits
  // e.g. SSD1331
  RGB332,

  // 12bpp, horizontal 2-pixel pack, high-to-low, big-endian
  // e.g. ST7789
  RGB444_HPACK2_H2L_BE,

  // 16bpp, big-endian
  RGB565_BE,

  // 18bpp, unpacked, 8bit left-aligned, big-endian
  RGB666_UNPACK_LA8_BE,

  // 18bpp, unpacked, 8bit right-aligned, big-endian
  RGB666_UNPACK_RA8_BE,

  // Number of formats (not a valid format)
  NUM_FORMATS,
};

static const char* INTERFACE_FORMAT_NAMES[] = {
    "Off",    "GRAY1",  "RGB111",    "RGB332",
    "RGB444", "RGB565", "RGB666-LA", "RGB666-RA"};
static_assert(
    sizeof(INTERFACE_FORMAT_NAMES) / sizeof(INTERFACE_FORMAT_NAMES[0]) ==
        static_cast<size_t>(InterfaceFormat::NUM_FORMATS) + 1,
    "INTERFACE_FORMAT_NAMES size must match InterfaceFormat enum + 1");

//=============================================================================
// Trim mode (cropping of framebuffer content before scaling to DVI output)
//=============================================================================

enum class TrimMode : uint8_t {
  OFF,
  AUTO,
  CUSTOM,
  NUM_MODES,
};

static const char* TRIM_MODE_NAMES[] = {"Off", "Auto", "Custom"};
static_assert(sizeof(TRIM_MODE_NAMES) / sizeof(TRIM_MODE_NAMES[0]) ==
                  static_cast<size_t>(TrimMode::NUM_MODES),
              "TRIM_MODE_NAMES size must match TrimMode enum");

//=============================================================================
// Flip mode (horizontal/vertical mirroring of framebuffer content)
//=============================================================================

// bit[0]: Horizontal Flip, bit[1]: Vertical Flip
enum class FlipMode : uint8_t {
  OFF,
  FLIP_H,
  FLIP_V,
  FLIP_HV,
};

static const char* FLIP_MODE_NAMES[] = {"Off", "Horizontal", "Vertical",
                                        "Both"};
static_assert(sizeof(FLIP_MODE_NAMES) / sizeof(FLIP_MODE_NAMES[0]) ==
                  static_cast<size_t>(FlipMode::FLIP_HV) + 1,
              "FLIP_MODE_NAMES size must match FlipMode enum");

//=============================================================================
// Scale mode (scaling of framebuffer content to DVI output)
//=============================================================================

enum class ScaleMode : uint8_t {
  OFF,       // 1:1 pixel mapping; black padding around the image
  INTEGRAL,  // Scale by integer factor; black padding around the image
  FIT,       // Letterbox/pillarbox to preserve aspect ratio
  STRETCH,   // Stretch to fill the full DVI area (ignores aspect ratio)
  NUM_MODES,
};

static const char* SCALE_MODE_NAMES[] = {"Off", "Integral", "Fit", "Stretch"};
static_assert(sizeof(SCALE_MODE_NAMES) / sizeof(SCALE_MODE_NAMES[0]) ==
                  static_cast<size_t>(ScaleMode::NUM_MODES),
              "SCALE_MODE_NAMES size must match ScaleMode enum");

//=============================================================================
// Configuration presets (predefined controller configs for common devices)
//=============================================================================

enum class ConfigPreset : uint8_t {
  ILI9341,
  ILI9342,
  ILI9488,
  SSD1306,
  SSD1331,
  KS0108,
  ST7735,
  ST7789,
  TEXT_0802,
  TEXT_1602,
  TEXT_1604,
  TEXT_2004,
  ARDUBOY,
  ESPBOY,
  M5STACK_CORES3,
  PICOPAD,
  PICOSYSTEM,
  THUMBY,
  TINYJOYPAD,
  WIO_TERMINAL,
  XIAMOCON,
  NUM_PRESETS,
};

static const char* CONFIG_PRESET_NAMES[] = {
    "ILI9341",   "ILI9342",    "ILI9488", "SSD1306",    "SSD1331",
    "KS0108",   "ST7735",     "ST7789",  "Text 8x2",   "Text 16x2",
    "Text 16x4", "Text 20x4",  "Arduboy", "ESPboy",     "M5Stack CoreS3",
    "PicoPad",   "PicoSystem", "Thumby",  "TinyJoypad", "Wio Terminal",
    "Xiamocon",
};
static_assert(sizeof(CONFIG_PRESET_NAMES) / sizeof(CONFIG_PRESET_NAMES[0]) ==
                  static_cast<size_t>(ConfigPreset::NUM_PRESETS),
              "CONFIG_PRESET_NAMES size must match ConfigPreset enum");

//=============================================================================
// Configuration structure
//=============================================================================
struct LcdTapConfig {
  ControllerFamily controllerFamily;
  BusType busInterface;

  // 7-bit I2C slave address the host must be configured to respond on.
  // Only meaningful when busInterface == BusType::I2C.
  uint8_t i2cSlaveAddr;

  uint16_t buffWidth;
  uint16_t buffHeight;

  // Character-display geometry (ControllerFamily::ST7032 only). buffWidth /
  // buffHeight are derived from these by normalizeConfig() and any
  // user-supplied values are overridden.
  uint8_t textCols;       // visible columns: 2..40, even
  uint8_t textRows;       // visible rows: 1, 2 or 4
  uint8_t textCgramArea;  // OPR2:OPR1 CGROM/CGRAM option (0..3)

  bool inverted;  // true: INVON→non-inverted / INVOFF→inverted
  bool swapRB;    // true: invert cachedBGR (swap R and B channels)

  // When true, fillScanline() renders pixels regardless of the sleeping /
  // displayOn state set by the LCD controller commands.
  bool forcePowerOn;

  // -1 = Off (interfaceFormat follows COLMOD/SETREMAP);
  // 0..NUM_FORMATS-1 = forced pixel format regardless of COLMOD/SETREMAP.
  int8_t interfaceFormatOverride;

  TrimMode trimMode;
  uint16_t trimX;
  uint16_t trimY;
  uint16_t trimWidth;
  uint16_t trimHeight;

  FlipMode flipMode;

  uint16_t dviWidth;   // DVI active area width (pixels)
  uint16_t dviHeight;  // DVI active area height (lines)
  ScaleMode scaleMode;
  uint8_t outputRotation;  // 0:none, 1:90°CW, 2:180°, 3:270°CW
};

enum class Configs : uint8_t {
  CTRL_FAMILY,
  BUS_INTERFACE,
  I2C_ADDR,
  BUFF_WIDTH,
  BUFF_HEIGHT,
  TEXT_COLS,
  TEXT_ROWS,
  TEXT_CGRAM_AREA,
  TRIM_MODE,
  TRIM_X,
  TRIM_Y,
  TRIM_WIDTH,
  TRIM_HEIGHT,
  FLIP_MODE,
  INVERTED,
  SWAP_RB,
  FORCE_PWR_ON,
  INTF_FMT_OVR,
  OUTPUT_ROT,
  SCALE_MODE,
  NUM_CONFIGS,
};

static const char* CONFIG_IDS[] = {
    "ctrlFamily", "busInterface", "i2cAddr",       "buffWidth", "buffHeight",
    "textCols",   "textRows",     "textCgramArea", "trimMode",  "trimX",
    "trimY",      "trimWidth",    "trimHeight",    "flipMode",  "inverted",
    "swapRB",     "forcePwrOn",   "intfFmtOvr",    "outputRot", "scaleMode"};

static_assert(sizeof(CONFIG_IDS) / sizeof(CONFIG_IDS[0]) ==
                  static_cast<size_t>(Configs::NUM_CONFIGS),
              "CONFIG_IDS size must match Configs enum");

enum class ValueType : uint8_t {
  INT16,
  BOOL,
  ENUM,
  HEX,  // INT16 semantics, displayed as 0xNN
};

struct ConfigEntry {
  ValueType type;        // Value type
  const char* name;      // Item label
  const char* unit;      // Unit string shown after the value (e.g. "px", "deg")
  const char** options;  // Display strings for ENUM type (nullptr otherwise)
  int16_t min;           // Minimum value (INTEGER / ENUM index)
  int16_t max;           // Maximum value (INTEGER / ENUM index)
  int16_t step;          // Increment per key press (INTEGER / ENUM)
  int16_t value;         // Current value; for ACTION: OSD_ACTION_XXX
  int16_t enableKeyId;
  int16_t enableKeyValueMin;
  int16_t enableKeyValueMax;
};

//=============================================================================
// Common string tables
//=============================================================================

static const char* ON_OFF_NAMES[] = {"Off", "On"};
static const char* ROTATION_NAMES[] = {"0", "90", "180", "270"};
static const char* ST7032_ROWS_NAMES[] = {"1", "2", "4"};
static const char* ST7032_CGRAM_NAMES[] = {"0x0-0xF", "0x0-0x7", "0x0-0x5",
                                           "None"};

//=============================================================================
// Get default configuration
// Writes default values for the specified controller into cfg.
// Override fields as needed before passing to the LcdTap constructor.
//=============================================================================
void getDefaultConfig(ControllerFamily type, LcdTapConfig* cfg);

//=============================================================================
// Normalize configuration
// Clamps controller-specific fields to valid ranges. For ST7032 this also
// derives buffWidth/buffHeight from textCols/textRows, overriding any
// user-supplied framebuffer size. Called by the LcdTap constructor,
// LcdTap::updateConfig() and getPresetConfig().
//=============================================================================
void normalizeConfig(LcdTapConfig* cfg);

//=============================================================================
// Get default interface format for a given controller family
//=============================================================================
InterfaceFormat getDefaultInterfaceFormat(ControllerFamily type);

//=============================================================================
// Configuration entry access
//=============================================================================
void getConfigEntryById(Configs config, ConfigEntry* e);
int16_t getConfigValueById(const LcdTapConfig& cfg, Configs config);
void setConfigValueById(LcdTapConfig* cfg, Configs config, int16_t value);
void formatConfigValue(char* buf, int bufLen, const ConfigEntry& item);

// Find a config item by its CONFIG_IDS string key.
// Returns Configs::NUM_CONFIGS when the key matches no item.
// Not reentrant: keeps a cursor so that keys arriving in CONFIG_IDS order
// (as setparams sends them) hit on the first comparison.
Configs findConfigByKey(const char* key);

//=============================================================================
// Get configuration preset
// Writes a predefined configuration for a common device into cfg.
//=============================================================================
void setPresetRotationOffset(uint8_t offset);
void getPresetConfig(ConfigPreset preset, LcdTapConfig* cfg);

//=============================================================================
// Command dump
//=============================================================================
struct DumpConfig {
  // reserved for future use
};

DumpConfig getDefaultDumpConfig();

}  // namespace lcdtap

#endif
