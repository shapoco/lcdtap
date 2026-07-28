#pragma once

#include <cstddef>
#include <lcdtap/lcdtap.hpp>

namespace lcdtap {

// ControllerBase — Common internal implementation base class (PIMPL body)
//
// Holds the common state and logic for display controllers that have a
// framebuffer and are driven by rectangle addressing (CASET/RASET) + RAMWR.
// Controller-specific parts (command codes, MADCTL, etc.) are implemented
// in derived classes.
class ControllerBase {
 public:
  virtual ~ControllerBase() = default;

  // --- Common configuration and state ---
  LcdTapConfig config;
  HostInterface host;
  Status status;
  bool hwReset;

  uint16_t*
      frameBuffer;  // lcdWidth × lcdHeight × sizeof(uint16_t) RGB565 buffer

  // Display control state
  bool sleeping;
  bool displayOn;
  bool inverted;
  bool writeProtected = false;
  InterfaceFormat interfaceFormat;

  // Command state machine
  uint8_t currentCmd;
  uint8_t cmdDataLen;  // number of data bytes received for the current command

  // RAMWR addressing
  uint16_t casetXS;     // CASET start column (logical coordinate)
  uint16_t casetXE;     // CASET end column   (logical coordinate, inclusive)
  uint16_t rasetYS;     // RASET start row    (logical coordinate)
  uint16_t rasetYE;     // RASET end row      (logical coordinate, inclusive)
  uint16_t ramwrX;      // current write position X (logical coordinate)
  uint16_t ramwrY;      // current write position Y (logical coordinate)
  uint8_t ramwrBuf[3];  // partial-byte accumulation / command data temp (up to
                        // 3 bytes)
  uint8_t ramwrBufLen;

  // Scaling parameters (computed in the constructor)
  uint16_t outDestX;  // LCD display area start X on DVI
  uint16_t outDestY;  // LCD display area start Y on DVI
  uint16_t outDestW;  // LCD display area width on DVI
  uint16_t outDestH;  // LCD display area height on DVI
  uint16_t outSrcX;  // LCD framebuffer start X for scaling (logical coordinate)
  uint16_t outSrcY;  // LCD framebuffer start Y for scaling (logical coordinate)
  uint16_t outSrcW;  // LCD framebuffer width for scaling (logical coordinate)
  uint16_t outSrcH;  // LCD framebuffer height for scaling (logical coordinate)
  uint32_t outSrcStepH;  // horizontal fixed-point step (16.16 format: lcdW<<16
                         // / outDestW)
  uint32_t outSrcStepV;  // vertical   fixed-point step (16.16 format: lcdH<<16
                         // / outDestH)
  uint8_t outputRotation;  // output rotation 0..3 (0:none, 1:90°CW, 2:180°,
                           // 3:270°CW)

  // RAMWR write cache (updated by updateWriteCache())
  bool cachedBGR;
  bool cachedLittleEndian;  // true = pixel bytes LSB first (RAMCTRL ENDIAN bit)
  int32_t cachedHOffset, cachedHStep;
  int32_t cachedVOffset, cachedVStep;
  uint16_t cachedInverter;
  uint16_t* writePtr;

  // Dirty-line tracking (see LcdTap::setDirtyTracking). One byte per physical
  // framebuffer row; bit k covers columns [64k, 64k+63]. Bits are set only
  // when a write actually changes a pixel value. presentEpoch counts output
  // changes that bypass the framebuffer (inversion, sleep, scaling, resets).
  bool dirtyTracking = false;
  uint32_t presentEpoch = 0;
  uint8_t dirtyMap[LcdTap::MAX_DIRTY_ROWS];

  // Debug statistics: opcodes that fell through to a controller's
  // unknown-command branch. lastUnknownCmd is only meaningful while
  // unknownCmdCount != 0.
  uint32_t unknownCmdCount = 0;
  uint8_t lastUnknownCmd = 0;

  // Per-call accumulator for the RAMWR write loop. diff collects XORs of
  // old/new pixel values within the current 64px segment; segBits collects
  // the dirty segments (in logical x) of the current logical row.
  struct DirtyAcc {
    uint32_t diff;
    uint32_t segBits;
  };

  // --- Controller-specific virtual interface ---

  // Logical width/height taking MADCTL MV into account
  virtual uint16_t logicalWidth() const = 0;
  virtual uint16_t logicalHeight() const = 0;

  // Logical coordinates → physical buffer index
  [[gnu::always_inline]] uint32_t physIndex(uint32_t lcol, uint32_t lrow) {
    return lcol * cachedHStep + cachedHOffset + lrow * cachedVStep +
           cachedVOffset;
  }

  // Update cache on MADCTL change or RAMWR start
  virtual void updateWriteCache() = 0;

  // Soft reset (initialise controller-specific registers, then call
  // resetCommon())
  virtual void softReset() = 0;

  // Process a command byte
  virtual void dispatchCommand(uint8_t cmd) = 0;

  // Process a non-RAMWR command data byte
  virtual void feedDataByte(uint8_t byte) = 0;

  // Returns true if the current command is a RAM write (RAMWR equivalent)
  virtual bool isRamWriteCommand() const = 0;

  // --- Common implementation ---

  // Compute scaling parameters (call inside the constructor)
  void calcScaleParams();

  // Log output helper
  void log(const char* msg) const;

  // Reset common fields (called from the derived class softReset())
  void resetCommon();

  // Set the output inversion state (true = inverted, false = normal)
  void setInverted(bool inv);

  // Expand the trim area to include the specified rectangle (logical
  // coordinates)
  void expandTrimX(uint16_t x0, uint16_t x1);

  // Expand the trim area to include the specified rectangle (logical
  // coordinates)
  void expandTrimY(uint16_t y0, uint16_t y1);

  // --- Dirty marking (out-of-line, cold relative to the pixel loop) ---

  void bumpEpoch() { ++presentEpoch; }

  void noteUnknownCommand(uint8_t cmd) {
    ++unknownCmdCount;
    lastUnknownCmd = cmd;
  }

  // OR the segment bits covering [physCol0, physCol1] into rows
  // [physRow0, physRow1] of dirtyMap. Coordinates are physical and clamped.
  void markDirtyRect(uint32_t physRow0, uint32_t physRow1, uint32_t physCol0,
                     uint32_t physCol1);

  // Mark a logical-coordinate rectangle dirty (translated through
  // physIndex(); the mapping is axis-aligned so the two corners suffice).
  void markLogicalRectDirty(uint32_t lx0, uint32_t lx1, uint32_t ly0,
                            uint32_t ly1);

  // Mark the segments of one completed logical row. segBits are 64px segment
  // bits in logical x; each is clipped to the CASET window and translated to
  // physical rows/columns.
  void markLogicalRowDirty(uint32_t ly, uint32_t segBits);

  void markAllDirty();

  // Fold the current segment's diff accumulation into segBits. Called at
  // 64px boundaries and at row/chunk ends; ramwrX must point one past the
  // last written pixel.
  LCDTAP_INLINE void flushDirtySeg(DirtyAcc& acc) {
    if (acc.diff != 0) {
      acc.segBits |= 1u << ((ramwrX - 1u) >> 6);
      acc.diff = 0;
    }
  }

  // Fold and publish the accumulated segments of the current logical row.
  LCDTAP_INLINE void flushDirtyRow(DirtyAcc& acc) {
    flushDirtySeg(acc);
    if (acc.segBits != 0) {
      markLogicalRowDirty(ramwrY, acc.segBits);
      acc.segBits = 0;
    }
  }

  // Write one RGB565 pixel to the framebuffer (respects MADCTL BGR).
  // kDirty=false compiles to the historical body with zero extra work; the
  // dirty variant additionally XORs the old pixel into acc.
  template <bool kDirty>
  LCDTAP_INLINE void writePixelRgb565(uint_fast16_t px, DirtyAcc& acc) {
    if (cachedBGR) {  // BGR: swap R[15:11] and B[4:0]
      px = ((px << 11) & 0xF800u) | (px & 0x07E0u) | ((px >> 11) & 0x1Fu);
    }
    px ^= cachedInverter;
    if (kDirty) {
      acc.diff |= static_cast<uint32_t>(*writePtr ^ px);
    }
    *writePtr = static_cast<uint16_t>(px);
    writePtr += cachedHStep;
    ++ramwrX;
    if (kDirty && (ramwrX & 63u) == 0u) flushDirtySeg(acc);
    if (ramwrX > casetXE) {
      if (kDirty) flushDirtyRow(acc);
      ramwrX = casetXS;
      if (++ramwrY > rasetYE) {
        ramwrY = rasetYS;
      }
      writePtr = frameBuffer + physIndex(ramwrX, ramwrY);
    }
  }
  // Process all RAMWR data at once (moves switch(interfaceFormat) outside the
  // loop) Override in derived classes to handle custom formats.
  virtual void processRamwrData(const uint8_t* data, uint32_t numBytes,
                                uint32_t stride);

  // Shared body of processRamwrData, compiled once per dirty-tracking state
  // so the tracking-off path keeps its historical codegen.
  template <bool kDirty>
  void processRamwrDataImpl(const uint8_t* data, uint32_t numBytes,
                            uint32_t stride);

  // Run-length fast paths for the two hot RAMWR formats. The per-pixel
  // writePixelRgb565() path cannot keep the write state in registers: the
  // framebuffer store aliases the uint16_t members of this class, so the
  // compiler reloads writePtr/ramwrX/cachedInverter every pixel. These
  // helpers split the pixel stream into row-bounded runs (64px
  // segment-bounded when kDirty) so the wrap checks and the member traffic
  // leave the inner loop. kStride is the compile-time input byte step
  // (0 = use strideArg at runtime). always_inline: the bodies must land in
  // whichever function calls them — see the *RunRing stamps below.
  template <bool kDirty, uint32_t kStride>
  LCDTAP_INLINE void ramwrRgb565RunImpl(const uint8_t* data, uint32_t numPixels,
                                        uint32_t strideArg, DirtyAcc& acc);

  // numGroups counts complete 3-byte groups (2 pixels each); leftover bytes
  // stay in ramwrBuf and are handled by the caller.
  template <bool kDirty, uint32_t kStride>
  LCDTAP_INLINE void ramwrRgb444RunImpl(const uint8_t* data, uint32_t numGroups,
                                        uint32_t strideArg, DirtyAcc& acc);

  // Non-template stamps of the ring-word input stride (sizeof(uint32_t)) --
  // the combination every RP2350 input driver uses. Stamped as plain
  // functions because they must be SRAM-resident (LCDTAP_RAM_FUNC) and GCC
  // silently drops section attributes on template instantiations
  // (GCC PR 70435).
  void ramwrRgb565RunRing(const uint8_t* data, uint32_t numPixels,
                          DirtyAcc& acc);
  void ramwrRgb565RunRingDirty(const uint8_t* data, uint32_t numPixels,
                               DirtyAcc& acc);
  void ramwrRgb444RunRing(const uint8_t* data, uint32_t numGroups,
                          DirtyAcc& acc);
  void ramwrRgb444RunRingDirty(const uint8_t* data, uint32_t numGroups,
                               DirtyAcc& acc);

  // Process a data byte stream: RAMWR data is handled in bulk, others byte by
  // byte
  void feedData(const uint8_t* data, uint32_t numBytes, uint32_t stride = 1);
};

}  // namespace lcdtap
