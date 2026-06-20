#pragma once

#include <lcdtap/devices/st7789.hpp>

#include "spi_display_base.hpp"

namespace lcdtap {

class St7789Controller final : public SpiDisplayBase {
 protected:
  void onFeedDataByte(uint8_t byte) override;
  InterfaceFormat colmodToFormat(uint8_t fmt) const override;
};

}  // namespace lcdtap
