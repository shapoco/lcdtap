#pragma once

#include <lcdtap/devices/st7789.hpp>

#include "controller_base.hpp"

namespace lcdtap {

class St7789Controller final : public ControllerBase {
 public:
  uint8_t madctl;

  uint16_t logicalWidth() const override;
  uint16_t logicalHeight() const override;
  uint32_t physIndex(uint32_t lcol, uint32_t lrow) const override;
  void updateWriteCache() override;
  void softReset() override;
  void dispatchCommand(uint8_t cmd) override;
  void feedDataByte(uint8_t byte) override;
  bool isRamWriteCommand() const override;
};

}  // namespace lcdtap
