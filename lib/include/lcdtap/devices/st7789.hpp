#pragma once

#include <cstdint>

namespace lcdtap::st7789 {

static constexpr uint8_t CMD_NOP = 0x00;
static constexpr uint8_t CMD_SWRESET = 0x01;
static constexpr uint8_t CMD_SLPOUT = 0x11;
static constexpr uint8_t CMD_INVOFF = 0x20;
static constexpr uint8_t CMD_INVON = 0x21;
static constexpr uint8_t CMD_DISPON = 0x29;
static constexpr uint8_t CMD_CASET = 0x2A;
static constexpr uint8_t CMD_RASET = 0x2B;
static constexpr uint8_t CMD_RAMWR = 0x2C;
static constexpr uint8_t CMD_MADCTL = 0x36;
static constexpr uint8_t CMD_COLMOD = 0x3A;

}  // namespace lcdtap::st7789
