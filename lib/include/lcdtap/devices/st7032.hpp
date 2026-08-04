#pragma once

#include <cstdint>

// ST7032 command constants (HD44780/ST7066U-compatible instruction set with
// the ST7032 extension-mode Function Set).
// Note: all commands are single RS=0 bytes decoded by their highest set bit.
//       RS=1 bytes are always DDRAM/CGRAM data.
//       Instruction table 1 (IS=1) commands are accepted but ignored.

namespace lcdtap {
namespace st7032 {

// Clear display: DDRAM <- 0x20, AC <- 0, shift <- 0, I/D <- 1
static constexpr uint8_t CMD_CLEAR_DISPLAY = 0x01;

// Return home: AC <- 0, shift <- 0 (0x02-0x03, bit0 = don't care)
static constexpr uint8_t CMD_RETURN_HOME = 0x02;

// Entry mode set: 0b0000_01xx
static constexpr uint8_t CMD_ENTRY_MODE = 0x04;
static constexpr uint8_t ENTRY_ID = 0x02;  // 1: increment AC on data R/W
static constexpr uint8_t ENTRY_S = 0x01;   // 1: shift display on data write

// Display ON/OFF: 0b0000_1xxx
static constexpr uint8_t CMD_DISPLAY_ONOFF = 0x08;
static constexpr uint8_t DISPLAY_D = 0x04;  // 1: display on
static constexpr uint8_t DISPLAY_C = 0x02;  // 1: cursor on (8th glyph line)
static constexpr uint8_t DISPLAY_B = 0x01;  // 1: cursor blink

// Cursor or display shift (IS=0 only): 0b0001_xxxx
static constexpr uint8_t CMD_SHIFT = 0x10;
static constexpr uint8_t SHIFT_SC = 0x08;  // 1: shift display, 0: move cursor
static constexpr uint8_t SHIFT_RL = 0x04;  // 1: right, 0: left

// Function set: 0b001x_xxxx
static constexpr uint8_t CMD_FUNCTION_SET = 0x20;
static constexpr uint8_t FUNC_DL = 0x10;  // 1: 8-bit bus, 0: 4-bit bus
static constexpr uint8_t FUNC_N = 0x08;   // 1: 2-line mode, 0: 1-line mode
static constexpr uint8_t FUNC_DH = 0x04;  // 1: double-height font (N=0 only)
static constexpr uint8_t FUNC_IS = 0x01;  // 1: instruction table 1 (ignored)

// Set CGRAM address (IS=0 only): 0b01xx_xxxx
static constexpr uint8_t CMD_SET_CGRAM_ADDR = 0x40;
static constexpr uint8_t CGRAM_ADDR_MASK = 0x3F;

// Set DDRAM address: 0b1xxx_xxxx
static constexpr uint8_t CMD_SET_DDRAM_ADDR = 0x80;
static constexpr uint8_t DDRAM_ADDR_MASK = 0x7F;

}  // namespace st7032
}  // namespace lcdtap
