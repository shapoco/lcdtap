#pragma once

#include "controller_base.hpp"

namespace lcdtap {

// SSD1331 controller implementation
//
// Key differences from ST7789:
// - DCX=0 bytes carry both commands and their parameters (like SSD1306)
// - DCX=1 bytes are always GRAM data; isRamWriteCommand() always returns true
// - No explicit RAMWR command; write position resets on SETROW
// - Supports rectangle-based addressing (SETCOLUMN/SETROW)
// - Supports hardware drawing commands (DRAWLINE, DRAWRECT, COPY, etc.)
// - Color depth is controlled by SETREMAP bits[7:6] (RGB332 or RGB565_BE)
class Ssd1331Controller : public ControllerBase {
 public:
  uint8_t remap;           // SETREMAP register value
  bool fillEnabled;        // fill enable flag (FILLENABLE command)
  uint8_t expectedParams;  // remaining parameter bytes for the current command
  uint8_t cmdBuf[10];  // parameter accumulation buffer (max 10 for DRAWRECT)
  uint8_t cmdBufLen;   // number of bytes stored in cmdBuf

  uint16_t logicalWidth() const override;
  uint16_t logicalHeight() const override;
  void updateWriteCache() override;
  void softReset() override;
  void dispatchCommand(uint8_t cmd) override;
  void feedDataByte(uint8_t byte) override;  // no-op: all DCX=1 is GRAM data
  bool isRamWriteCommand() const override;   // always true

 private:
  void execCommand();
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
  void fillRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
  void dimWindow(int16_t x0, int16_t y0, int16_t x1, int16_t y1);
  void copyRegion(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t xd,
                  int16_t yd);
  void setPixelAt(int16_t x, int16_t y, uint16_t color);
  uint16_t makeColor(uint8_t r6, uint8_t g6, uint8_t b6) const;
};

}  // namespace lcdtap
