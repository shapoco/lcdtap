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

  uint16_t* framebuf;  // lcdWidth × lcdHeight × sizeof(uint16_t) RGB565 buffer

  // Display control state
  bool sleeping;
  bool displayOn;
  bool inverted;
  PixelFormat pixelFormat;

  // Command state machine
  uint8_t currentCmd;
  uint8_t cmdDataLen;  // number of data bytes received for the current command

  // RAMWR addressing
  uint16_t casetXS;  // CASET start column (logical coordinate)
  uint16_t casetXE;  // CASET end column   (logical coordinate, inclusive)
  uint16_t rasetYS;  // RASET start row    (logical coordinate)
  uint16_t rasetYE;  // RASET end row      (logical coordinate, inclusive)
  uint16_t ramwrX;   // current write position X (logical coordinate)
  uint16_t ramwrY;   // current write position Y (logical coordinate)
  uint8_t
      ramwrBuf[3];  // partial-byte accumulation / command data temp (up to 3 bytes)
  uint8_t ramwrBufLen;

  // Scaling parameters (computed in the constructor)
  uint16_t displayX;  // LCD display area start X on DVI
  uint16_t displayY;  // LCD display area start Y on DVI
  uint16_t displayW;  // LCD display area width on DVI
  uint16_t displayH;  // LCD display area height on DVI
  uint32_t hStep;  // horizontal fixed-point step (16.16 format: lcdW<<16 / displayW)
  uint32_t vStep;  // vertical   fixed-point step (16.16 format: lcdH<<16 / displayH)
  uint8_t outputRotation;  // output rotation 0..3 (0:none, 1:90°CW, 2:180°, 3:270°CW)

  // RAMWR write cache (updated by updateWriteCache())
  bool cachedBGR;
  int32_t cachedHOffset, cachedHStep;
  int32_t cachedVOffset, cachedVStep;
  uint16_t* writePtr;

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

  // Soft reset (initialise controller-specific registers, then call resetCommon())
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

  // Write one RGB565 pixel to the framebuffer (respects MADCTL BGR)
  [[gnu::always_inline]] void writePixelRgb565(uint16_t px) {
    if (cachedBGR) {  // BGR: swap R[15:11] and B[4:0]
      uint16_t r = (px >> 11) & 0x1Fu;
      uint16_t g = (px >> 5) & 0x3Fu;
      uint16_t b = px & 0x1Fu;
      px = static_cast<uint16_t>((b << 11) | (g << 5) | r);
    }
    *writePtr = px;
    writePtr += cachedHStep;
    if (++ramwrX > casetXE) {
      ramwrX = casetXS;
      if (++ramwrY > rasetYE) {
        ramwrY = rasetYS;
      }
      writePtr = framebuf + physIndex(ramwrX, ramwrY);
    }
  }
  // Process all RAMWR data at once (moves switch(pixelFormat) outside the loop)
  // Override in derived classes to handle custom formats.
  virtual void processRamwrData(const uint8_t* data, uint32_t numBytes, uint32_t stride);

  // Process a data byte stream: RAMWR data is handled in bulk, others byte by byte
  void feedData(const uint8_t* data, uint32_t numBytes, uint32_t stride = 1);
};

}  // namespace lcdtap
