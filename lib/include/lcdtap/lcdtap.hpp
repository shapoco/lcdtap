#pragma once

// LcdTap — Core Library
// Interprets LCD controller commands and generates DVI-D signal content.
//
// Usage:
//   #include <lcdtap/lcdtap.hpp>

#include <cstddef>
#include <cstdint>

#define LCDTAP_INLINE inline __attribute__((always_inline))

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
// LCD controller type
//=============================================================================
enum class ControllerType : uint8_t {
  ST7789,
  SSD1306,
  SSD1331,
};

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

//=============================================================================
// Scaling mode (when LCD resolution differs from DVI active area)
//=============================================================================
enum class ScaleMode : uint8_t {
  STRETCH,        // Stretch to fill the full DVI area (ignores aspect ratio)
  FIT,            // Letterbox/pillarbox to preserve aspect ratio
  PIXEL_PERFECT,  // Scale by integer factor; black padding around the image
};

//=============================================================================
// Configuration structure
//=============================================================================
struct LcdTapConfig {
  ControllerType controller;

  uint16_t lcdWidth;
  uint16_t lcdHeight;

  bool inverted;  // true: INVON→non-inverted / INVOFF→inverted
  bool swapRB;    // true: invert cachedBGR (swap R and B channels)

  // When true, fillScanline() renders pixels regardless of the sleeping /
  // displayOn state set by the LCD controller commands.
  bool forcePowerOn;

  // -1 = Off (interfaceFormat follows COLMOD/SETREMAP);
  // 0..NUM_FORMATS-1 = forced pixel format regardless of COLMOD/SETREMAP.
  int8_t interfaceFormatOverride;

  uint16_t dviWidth;   // DVI active area width (pixels)
  uint16_t dviHeight;  // DVI active area height (lines)
  ScaleMode scaleMode;
  uint8_t outputRotation;  // 0:none, 1:90°CW, 2:180°, 3:270°CW
};

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
void getDefaultConfig(ControllerType type, LcdTapConfig* cfg);

InterfaceFormat getDefaultInterfaceFormat(ControllerType type);

// Returns a short display name for the given InterfaceFormat (e.g. "RGB565").
const char* getShortInterfaceFormatName(InterfaceFormat fmt);

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
