#pragma once

#include "controller_base.hpp"

namespace lcdtap {

// KS0108 (SBN0064G-compatible) dual-chip graphic LCD controller (SG12864).
//
// Key differences from SSD1306:
// - Two 64x64 chips side by side; inputCs selects the target chip(s)
//   (bit0 = chip 0 / columns 0-63, bit1 = chip 1 / columns 64-127).
//   Each chip has its own page / column / start-line / on-off registers.
// - D/I=1 bytes are always display RAM data; isRamWriteCommand() is
//   always true. The column address auto-increments and wraps at 64
//   without crossing into the other chip.
// - The display start line (vertical scroll) is folded into the write-time
//   row mapping; on change the chip's framebuffer half is rotated so
//   already-drawn content scrolls accordingly.
// - Per-chip display off cannot be rendered: displayOn is the OR of both
//   chips (hosts normally switch both together).
// - No trim auto-expansion (same as ST7032): the full 128x64 area is
//   considered active.
class Ks0108Controller : public ControllerBase {
 public:
  static constexpr uint16_t CHIP_WIDTH = 64;
  static constexpr uint16_t CHIP_HEIGHT = 64;
  static constexpr int NUM_CHIPS = 2;

  struct ChipState {
    uint8_t page;       // X address (page, 0-7)
    uint8_t col;        // Y address (column, 0-63, auto-increment)
    uint8_t startLine;  // Z address (display start line, 0-63)
    bool on;            // display on/off
  };
  ChipState chips[NUM_CHIPS];

  uint16_t logicalWidth() const override;
  uint16_t logicalHeight() const override;
  void updateWriteCache() override;
  void softReset() override;
  void dispatchCommand(uint8_t cmd) override;
  void feedDataByte(uint8_t byte) override;
  bool isRamWriteCommand() const override;
  void processRamwrData(const uint8_t* data, uint32_t numBytes,
                        uint32_t stride) override;

 private:
  inline bool effectiveFlipH() const {
    return (static_cast<uint8_t>(config.flipMode) & 0x01u) != 0u;
  }
  inline bool effectiveFlipV() const {
    return (static_cast<uint8_t>(config.flipMode) & 0x02u) != 0u;
  }
  // Leftmost physical framebuffer column of a chip's half (FlipMode applied)
  inline uint16_t chipColBase(int chip) const {
    return CHIP_WIDTH * static_cast<uint16_t>(
                            effectiveFlipH() ? NUM_CHIPS - 1 - chip : chip);
  }
  void setStartLine(int chip, uint8_t newZ);
  void writeDataByte(int chip, uint8_t byte);
};

}  // namespace lcdtap
