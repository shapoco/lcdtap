#pragma once

#include "controller_base.hpp"

namespace lcdtap {

// SpiDisplayBase — Common base for SPI color display controllers
//
// Implements the shared command set for controllers that use CASET/RASET/RAMWR
// addressing with MADCTL orientation control (ST7789, ILI9341, etc.).
// Controller-specific extensions are provided via protected virtual hooks.
class SpiDisplayBase : public ControllerBase {
 public:
  static constexpr uint8_t MADCTL_BGR = 0x08;
  static constexpr uint8_t MADCTL_MV = 0x20;
  static constexpr uint8_t MADCTL_MX = 0x40;
  static constexpr uint8_t MADCTL_MY = 0x80;

  uint8_t madctl;

  inline uint8_t effectiveMadctl() const {
    uint8_t f = static_cast<uint8_t>(config.flipMode);
    uint8_t mask =
        ((f & 0x01u) ? MADCTL_MX : 0u) | ((f & 0x02u) ? MADCTL_MY : 0u);
    return madctl ^ mask;
  }
  uint16_t hwColStart;  // CASET start (hardware column coordinate)
  uint16_t hwColEnd;    // CASET end   (hardware column coordinate, inclusive)
  uint16_t hwRowStart;  // RASET start (hardware row coordinate)
  uint16_t hwRowEnd;    // RASET end   (hardware row coordinate, inclusive)

  uint16_t logicalWidth() const override;
  uint16_t logicalHeight() const override;
  void updateWriteCache() override;
  void softReset() override;
  void dispatchCommand(uint8_t cmd) override;
  void feedDataByte(uint8_t byte) override;
  bool isRamWriteCommand() const override;

 protected:
  // Called by dispatchCommand() for opcodes not handled by SpiDisplayBase
  virtual void onDispatchCommand(uint8_t cmd) {}

  // Called by feedDataByte() for data bytes of commands not handled by
  // SpiDisplayBase
  virtual void onFeedDataByte(uint8_t byte) {}

  // Converts COLMOD bits[2:0] to InterfaceFormat.
  // Returns InterfaceFormat::NUM_FORMATS for unrecognised codes.
  virtual InterfaceFormat colmodToFormat(uint8_t fmt) const;
};

}  // namespace lcdtap
