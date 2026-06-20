#pragma once

#include <lcdtap/devices/ili9341.hpp>

#include "spi_display_base.hpp"

namespace lcdtap {

// ILI9341 shares the full common command set with no extensions.
// All behaviour is provided by SpiDisplayBase.
class Ili9341Controller final : public SpiDisplayBase {};

}  // namespace lcdtap
