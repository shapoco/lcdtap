#pragma once

// LcdTap — Core Library
// Interprets LCD controller commands and generates DVI-D signal content.
//
// Usage:
//   #include <lcdtap/lcdtap.hpp>

#include <cstddef>
#include <cstdint>

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
};

//=============================================================================
// Pixel format (SPI input side — COLMOD equivalent)
//=============================================================================
enum class PixelFormat : uint8_t {
  MONO_VPACK = 0x00,  // 1bpp monochrome vertical 8-pixel pack (SSD1306)
  RGB444 = 0x03,      // 12bpp
  RGB565 = 0x05,      // 16bpp
  RGB666 = 0x06,      // 18bpp
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
  // --- LCD controller ---
  ControllerType controller;

  // --- SPI input (LCD) side ---
  uint16_t lcdWidth;
  uint16_t lcdHeight;
  PixelFormat pixelFormat;  // Initial pixel format (can be changed via COLMOD)

  // --- DVI output side ---
  uint16_t dviWidth;   // DVI active area width (pixels)
  uint16_t dviHeight;  // DVI active area height (lines)
  ScaleMode scaleMode;

  bool invertInvPolarity;  // true: INVON→non-inverted / INVOFF→inverted
  bool swapRB;             // true: invert cachedBGR (swap R and B channels)

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
};

}  // namespace lcdtap
