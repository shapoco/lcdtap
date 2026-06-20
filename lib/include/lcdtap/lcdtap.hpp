#pragma once

// LcdTap — Core Library
// Interprets LCD controller commands and generates DVI-D signal content.
//
// Usage:
//   #include <lcdtap/lcdtap.hpp>

#include <cstddef>
#include <cstdint>

#define LCDTAP_INLINE inline __attribute__((always_inline))

#define LCDTAP_CLIP(min, max, val) \
  ((val) < (min) ? (min) : ((val) > (max) ? (max) : (val)))

namespace lcdtap {

//=============================================================================
// Status codes
//=============================================================================
enum class Status : int {
  OK = 0,
  INVALID_PARAM,  // a field in config has an invalid value
  OUT_OF_MEMORY,  // alloc() returned nullptr
  NOT_READY,      // an operation was called after a constructor failure
};

//=============================================================================
// LCD controller families
//=============================================================================
enum class ControllerFamily : uint8_t {
  ST7789,
  SSD1306,
  SSD1331,
  ILI9341,
  NUM_CONTROLLERS,
};

static const char* CONTROLLER_NAMES[] = {"ST7789", "SSD1306", "SSD1331",
                                         "ILI9341"};
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
  NUM_INTERFACES,
};

static const char* INTERFACE_NAMES[] = {"I2C", "4-Line SPI", "3-Line SPI",
                                        "Parallel"};
static_assert(sizeof(INTERFACE_NAMES) / sizeof(INTERFACE_NAMES[0]) ==
                  static_cast<size_t>(BusType::NUM_INTERFACES),
              "INTERFACE_NAMES size must match lcdtap::BusType enum");

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
// Scale mode (scaling of framebuffer content to DVI output)
//=============================================================================

enum class ScaleMode : uint8_t {
  STRETCH,        // Stretch to fill the full DVI area (ignores aspect ratio)
  FIT,            // Letterbox/pillarbox to preserve aspect ratio
  PIXEL_PERFECT,  // Scale by integer factor; black padding around the image
  NUM_MODES,
};

static const char* SCALE_MODE_NAMES[] = {"Stretch", "Fit", "Dot-by-Dot"};
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
  ST7735,
  ST7789,
  ARDUBOY,
  M5STACK_CORES3,
  THUMBY,
  TINYJOYPAD,
  XIAMOCON,
  NUM_PRESETS,
};

static const char* CONFIG_PRESET_NAMES[] = {
    "ILI9341", "ILI9342", "ILI9488",        "SSD1306", "SSD1331",    "ST7735",
    "ST7789",  "Arduboy", "M5Stack CoreS3", "Thumby",  "TinyJoypad", "Xiamocon",
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

  uint16_t buffWidth;
  uint16_t buffHeight;

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

  uint16_t dviWidth;   // DVI active area width (pixels)
  uint16_t dviHeight;  // DVI active area height (lines)
  ScaleMode scaleMode;
  uint8_t outputRotation;  // 0:none, 1:90°CW, 2:180°, 3:270°CW
};

enum class ConfigId : uint8_t {
  CTRL_FAMILY,
  BUS_INTERFACE,
  BUFF_WIDTH,
  BUFF_HEIGHT,
  TRIM_MODE,
  TRIM_X,
  TRIM_Y,
  TRIM_WIDTH,
  TRIM_HEIGHT,
  INVERSE,
  SWAP_RB,
  OUTPUT_ROT,
  SCALE_MODE,
  FORCE_PWR_ON,
  INTF_FMT_OVR,
  NUM_CONFIGS,
};

enum class ValueType : uint8_t {
  INT16,
  BOOL,
  ENUM,
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

//=============================================================================
// Host interface
//=============================================================================
struct HostInterface {
  // --- Memory management (required) ---
  // Used for framebuffer allocation/deallocation.
  // Pass a custom allocator if you need to allocate in external memory (e.g.
  // PSRAM).
  void* (*alloc)(size_t size);
  void (*free)(void* ptr);

  // --- Notification callbacks (optional; nullptr to disable) ---

  // Debug log output (disabled when nullptr)
  void (*log)(void* userData, const char* message);

  void* userData;
};

//=============================================================================
// Get default configuration
// Writes default values for the specified controller into cfg.
// Override fields as needed before passing to the LcdTap constructor.
//=============================================================================
void getDefaultConfig(ControllerFamily type, LcdTapConfig* cfg);

//=============================================================================
// Get configuration preset
// Writes a predefined configuration for a common device into cfg.
//=============================================================================
void getPresetConfig(ConfigPreset preset, LcdTapConfig* cfg);

//=============================================================================
// Get default interface format for a given controller family
//=============================================================================
InterfaceFormat getDefaultInterfaceFormat(ControllerFamily type);

//=============================================================================
// Returns a short display name for the given InterfaceFormat (e.g. "RGB565").
//=============================================================================
const char* getShortInterfaceFormatName(InterfaceFormat fmt);

//=============================================================================
// Configuration entry access
//=============================================================================
void getConfigEntryById(ConfigId id, ConfigEntry* e);
int16_t getConfigValueById(const LcdTapConfig& cfg, ConfigId id);
void setConfigValueById(LcdTapConfig* cfg, ConfigId id, int16_t value);
void formatConfigValue(char* buf, int bufLen, const ConfigEntry& item);

//=============================================================================
// Command dump
//=============================================================================
struct DumpConfig {
  // reserved for future use
};

DumpConfig getDefaultDumpConfig();

enum class DumpState : uint8_t {
  WAIT,      // waiting for trigger
  ACTIVE,    // capturing
  COMPLETE,  // buffer full or aborted
};

//=============================================================================
// Main class
//=============================================================================
class LcdTap {
 public:
  LcdTap(const LcdTapConfig& config, const HostInterface& host);
  ~LcdTap();

  // Check status after a constructor failure
  Status getStatus() const;

  //--- SPI input ---

  // Hardware reset signal input (equivalent to RESX pin)
  // assert=true: hold in reset; false: release reset
  void inputReset(bool assert);

  // Command byte input (D/CX=Low)
  void inputCommand(uint8_t byte);

  // Data byte stream input (D/CX=High)
  void inputData(const uint8_t* data, uint32_t numBytes, uint32_t stride = 1);

  //--- DVI output ---

  // Writes pixel data for the specified line number into dst.
  // Format: RGB565 (1 pixel = uint16_t, R[15:11] G[10:5] B[4:0])
  // line range: 0 .. dviHeight - 1 (DVI coordinate system)
  // Scaling is handled internally by the library.
  // dst must point to a buffer that can hold dviWidth uint16_t values.
  void fillScanline(uint16_t line, uint16_t* dst) const;

  // Set display output rotation.
  // rot=0: no rotation (default)
  // rot=1: 90° clockwise. With FIT/PIXEL_PERFECT the aspect ratio is
  // transposed.
  // rot=2: 180° rotation. Aspect ratio is unchanged.
  // rot=3: 270° clockwise. With FIT/PIXEL_PERFECT
  // the aspect ratio is transposed.
  // Does not affect the controller's internal state.
  // Only the readout pattern of fillScanline() changes.
  void setOutputRotation(int rot);

  // Returns the current configuration.
  LcdTapConfig getConfig() const;

  // Update configuration and reallocate the framebuffer.
  // Returns OK on success, OUT_OF_MEMORY if the new framebuffer allocation
  // fails (the previous state is left intact in that case).
  Status updateConfig(const LcdTapConfig& cfg);

  // Returns true if the display is currently inverted.
  bool isOutputInverted() const;

  // Returns true if the R and B channels are swapped.
  bool isOutputSwapRB() const;

  //--- Command dump ---

  static constexpr int DUMP_BUFFER_SIZE = 256;
  static constexpr uint16_t DUMP_EVENT_HW_RESET = 0x8001u;

  // Clear buffer, load config, and transition to WAIT state.
  // If currently COMPLETE, this is the only way back to WAIT.
  void dumpStart(const DumpConfig& dumpCfg);

  DumpState dumpGetState() const;
  uint16_t dumpGetSize() const;

  // Transition WAIT→ACTIVE.
  void dumpForceTrigger();

  // Transition to COMPLETE unconditionally.
  void dumpAbort();

  const uint16_t* dumpGetBuffer() const;

  //--- Write protection ---

  // When true, RAM write commands (RAMWR) are silently discarded while all
  // other commands continue to be processed. Use before reading getFramebuf()
  // to prevent partial writes during readout.
  void setWriteProtected(bool protect);
  bool isWriteProtected() const;

  //--- Test / debug ---

  // Returns a direct write pointer to the framebuffer.
  // Format: lcdWidth × lcdHeight × sizeof(uint16_t) bytes
  //         (row-major RGB565, without MADCTL transform).
  uint16_t* getFramebuf();

  // Returns the computed output source region in physical buffer coordinates
  // (pre-rotation). These are the crop coordinates used by fillScanline().
  void getOutSrcRegion(uint16_t* x, uint16_t* y, uint16_t* w,
                       uint16_t* h) const;

  // Force-set the sleep/display-on state.
  // on=true: sets sleeping=false and displayOn=true, making fillScanline()
  // return non-black pixels. on=false: sets sleeping=true and returns a black
  // screen. Use this when you want to display a test pattern before the SPI
  // master sends SLPOUT/DISPON.
  void setDisplayOn(bool on);

 private:
  LcdTap(const LcdTap&) = delete;
  LcdTap& operator=(const LcdTap&) = delete;

  void* impl_;  // Hides internal implementation (PIMPL)

  DumpConfig dumpConfig_;
  DumpState dumpState_ = DumpState::ACTIVE;
  uint16_t dumpBuffer_[DUMP_BUFFER_SIZE];
  uint16_t dumpBuffSize_ = 0;

  inline void dumpPush(uint16_t value) {
    if (dumpBuffSize_ < DUMP_BUFFER_SIZE) dumpBuffer_[dumpBuffSize_++] = value;
    if (dumpBuffSize_ >= DUMP_BUFFER_SIZE) dumpState_ = DumpState::COMPLETE;
  }
};

}  // namespace lcdtap
