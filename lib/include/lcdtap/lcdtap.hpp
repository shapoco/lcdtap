#pragma once

// LcdTap — Core Library
// Interprets LCD controller commands and generates DVI-D signal content.
//
// Usage:
//   #include <lcdtap/lcdtap.hpp>

#include <cstddef>
#include <cstdint>

#include "lcdtap/config.hpp"

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
// Fixed-point scale factor for scaling calculations
//=============================================================================
static constexpr int FIXPT_PREC = 16;

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
// Command dump
//=============================================================================

enum class DumpState : uint8_t {
  WAIT,      // waiting for trigger
  ACTIVE,    // capturing
  COMPLETE,  // buffer full or aborted
};

//=============================================================================
// Debug statistics
//=============================================================================
// Presentation hint for a StatEntry value. All values are raw uint32 that
// wrap around at 2^32; formatting is left to the display side.
enum class StatFormat : uint8_t {
  DEC,   // plain decimal, optionally followed by unit
  HEX,   // byte value shown as 0xNN; value > 0xFF means "none yet"
  RATE,  // bytes per second, auto-scaled to B/s, KB/s or MB/s
};

// One name/value/unit tuple produced by a host-side stats provider and
// consumed by the OSD statistics view and the UART "getstats" command.
struct StatEntry {
  const char* name;
  uint32_t value;
  const char* unit;  // suffix for DEC values (e.g. "B", "s"); may be ""
  StatFormat fmt;
};

//=============================================================================
// Output mapping info (see LcdTap::getOutputMapInfo)
//=============================================================================
// Snapshot of the scaling parameters used by fillScanline(), for host-side
// partial-update pumps that need to map framebuffer regions to output
// regions (e.g. a USB display that is updated by dirty rectangles).
//
// Mapping formulas (identical to fillScanline):
//   active output rect: x in [destX, destX+destW), y in [destY, destY+destH)
//   srcLine = ((y - destY) * stepV) >> 16
//   rot=0: fb row = srcY + srcLine          rot=2: fb row = srcY+srcH-1-srcLine
//   rot=1: fb col = srcX + srcLine          rot=3: fb col = srcX+srcW-1-srcLine
// Along the line, the forward orientations (rot=0/3) sample source index
//   idx(x) = ((x - destX) * stepH) >> 16
// and the mirrored orientations (rot=2/1) are guaranteed to sample the exact
// mirror, idx(destX + destW-1 - x): mirrored spans can be derived from
// forward spans by reflection without rounding error.
// Everything outside the active rect is black, as is the whole screen while
// blanked is true.
struct OutputMapInfo {
  uint16_t destX, destY;  // active rect origin (destX pre-aligned to even)
  uint16_t destW, destH;  // active rect size
  uint16_t srcX, srcY;    // fb crop origin (physical, pre-rotation)
  uint16_t srcW, srcH;    // fb crop size
  uint32_t stepH, stepV;  // 16.16 fixed-point source step per output px/line
  uint8_t rotation;       // output rotation 0..3
  uint16_t fbWidth, fbHeight;  // physical framebuffer size
  bool blanked;                // true: fillScanline outputs black everywhere
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

  // Hardware reset signal input (equivalent to RST pin)
  // assert=true: hold in reset; false: release reset
  void inputReset(bool assert);

  // Command byte input (D/CX=Low)
  void inputCommand(uint8_t byte);

  // Data byte stream input (D/CX=High)
  void inputData(const uint8_t* data, uint32_t numBytes, uint32_t stride = 1);

  // Periodic service hook. Drives time-dependent controller behaviour such
  // as the ST7032 cursor blink; a no-op for other controllers. Call from the
  // host main loop (a ~100 ms or finer cadence is sufficient).
  void tick(uint32_t nowMs);

  //--- DVI output ---

  // Writes pixel data for the specified line number into dst.
  // Format: RGB565 (1 pixel = uint16_t, R[15:11] G[10:5] B[4:0])
  // line range: 0 .. dviHeight - 1 (DVI coordinate system)
  // Scaling is handled internally by the library.
  // dst must point to a buffer that can hold dviWidth uint16_t values.
  void fillScanline(uint16_t line, uint16_t* dst) const;

  // Set display output rotation.
  // rot=0: no rotation (default)
  // rot=1: 90° clockwise. With FIT/INTEGRAL the aspect ratio is
  // transposed.
  // rot=2: 180° rotation. Aspect ratio is unchanged.
  // rot=3: 270° clockwise. With FIT/INTEGRAL
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

  // Returns the current output screen size.
  void getOutputScreenSize(uint16_t* width, uint16_t* height) const;

  // Returns true if the display is currently inverted.
  bool isOutputInverted() const;

  // Returns true if the R and B channels are swapped.
  bool isOutputSwapRB() const;

  //--- Dirty-line tracking (for host-side partial output updates) ---

  // The framebuffer is tracked in physical coordinates as one byte per row;
  // bit k covers columns [64k, 64k+63]. A bit is set only when a write
  // actually changed a pixel value, so hosts that rewrite the full screen
  // every frame produce no dirty bits for unchanged content.
  static constexpr uint16_t DIRTY_SEG_PX = 64;
  static constexpr uint16_t MAX_DIRTY_ROWS = 512;

  // Enable/disable tracking. Enabling marks everything dirty. Tracking stays
  // inactive (isDirtyTrackingActive() == false) when the framebuffer exceeds
  // MAX_DIRTY_ROWS in either dimension; hosts must then treat every row as
  // permanently dirty.
  void setDirtyTracking(bool on);
  bool isDirtyTrackingActive() const;

  // One byte per physical framebuffer row (buffHeight rows). The caller
  // clears the bytes it has consumed; no locking is provided, so producer
  // (input processing) and consumer must run on the same core.
  uint8_t* dirtyMap();
  void markAllDirty();

  // Incremented whenever the output can change without a framebuffer write
  // (inversion, sleep/display-on, trim/scale/rotation changes, resets).
  // Hosts resend the whole screen when this value changes.
  uint32_t getPresentationEpoch() const;

  // Snapshot of the fillScanline() mapping parameters (see OutputMapInfo).
  void getOutputMapInfo(OutputMapInfo* out) const;

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

  //--- Debug statistics ---

  // Free-running 32-bit counters (wrap around at 2^32). Byte counters count
  // everything fed into inputCommand()/inputData(), regardless of reset or
  // error state. Hosts implement "reset" by baseline subtraction.
  uint32_t getRxCmdBytes() const { return rxCmdBytes_; }
  uint32_t getRxDataBytes() const { return rxDataBytes_; }
  uint32_t getHwResetCount() const { return hwResetCount_; }

  // Opcodes that fell through to the controller's unknown-command branch.
  // Both survive updateConfig(), including a controller-family swap.
  uint32_t getUnknownCmdCount() const;
  uint8_t getLastUnknownCmd() const;

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

  uint32_t rxCmdBytes_ = 0;
  uint32_t rxDataBytes_ = 0;
  uint32_t hwResetCount_ = 0;

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
