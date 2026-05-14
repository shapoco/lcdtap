#pragma once

#include "controller_base.hpp"

namespace lcdtap {

// SSD1306 controller implementation
//
// Key differences from ST7789:
// - DCX=0 bytes carry both commands and their parameters
// - DCX=1 bytes are always GDDRAM data (MONO_VPACK: 1 byte = vertical 8 pixels)
// - There is no explicit RAMWR command; isRamWriteCommand() always returns true
// - Addressing is page-based (horizontal / vertical / page modes)
class Ssd1306Controller : public ControllerBase {
 public:
  uint8_t ssdAddrMode;     // 0=horizontal, 1=vertical, 2=page (default)
  bool ssdSegmentRemap;    // true=A1 (col127→SEG0, horizontal flip)
  bool ssdComFlip;         // true=C8 (COM63→COM0, vertical flip)
  uint8_t expectedParams;  // remaining parameter bytes for the current command
  uint8_t pageColLow;      // page mode column address lower nibble
  uint8_t pageColHigh;     // page mode column address upper nibble

  uint16_t logicalWidth() const override;
  uint16_t logicalHeight() const override;
  void updateWriteCache() override;
  void softReset() override;
  void dispatchCommand(uint8_t cmd) override;
  void feedDataByte(uint8_t byte) override;
  bool isRamWriteCommand() const override;
  void processRamwrData(const uint8_t* data, uint32_t numBytes, uint32_t stride) override;

 private:
  void applyPageModeCol();
};

}  // namespace lcdtap
