#pragma once

#include <cstdint>

// KS0108 / SBN0064G command constants
// Note: the instruction set has only four commands, all encoded in the upper
//       bits of a single D/I=0 byte. D/I=1 bytes are always display RAM data
//       (GRAY1_VPACK8_H2L: 1 byte = vertical 8 pixels, bit0 = top).

namespace lcdtap {
namespace ks0108 {

// Display on/off (0011111X: bit0 = 1:on, 0:off)
static constexpr uint8_t CMD_DISPLAY_ONOFF_MASK = 0x01;
static constexpr uint8_t CMD_DISPLAY_ONOFF_BASE = 0x3E;

// Set Y (column) address (01YYYYYY, auto-incremented by data access)
static constexpr uint8_t CMD_SET_COL_MASK = 0x3F;
static constexpr uint8_t CMD_SET_COL_BASE = 0x40;

// Set X (page) address (10111XXX)
static constexpr uint8_t CMD_SET_PAGE_MASK = 0x07;
static constexpr uint8_t CMD_SET_PAGE_BASE = 0xB8;

// Set display start line Z (11ZZZZZZ, vertical scroll)
static constexpr uint8_t CMD_SET_START_LINE_MASK = 0x3F;
static constexpr uint8_t CMD_SET_START_LINE_BASE = 0xC0;

}  // namespace ks0108
}  // namespace lcdtap
