#pragma once

#include <cstdint>

// SSD1309 command constants
// Note: all commands with parameters are sent as DCX=0 bytes.
//       DCX=1 bytes are always GDDRAM data (MONO_VPACK).

namespace lcdtap {
namespace ssd1309 {

// Lower column address (for page addressing mode, 0x00-0x0F)
static constexpr uint8_t CMD_SET_LOWER_COL_MASK = 0x0F;
static constexpr uint8_t CMD_SET_LOWER_COL_BASE = 0x00;

// Upper column address (for page addressing mode, 0x10-0x1F)
static constexpr uint8_t CMD_SET_HIGHER_COL_MASK = 0x0F;
static constexpr uint8_t CMD_SET_HIGHER_COL_BASE = 0x10;

// Memory addressing mode (1-byte parameter)
// Parameter: 0x00=horizontal, 0x01=vertical, 0x02=page (default)
static constexpr uint8_t CMD_SET_ADDR_MODE = 0x20;

// Column address (2-byte parameter: start, end) — horizontal/vertical modes
static constexpr uint8_t CMD_SET_COL_ADDR = 0x21;

// Page address (2-byte parameter: start, end) — horizontal/vertical modes
static constexpr uint8_t CMD_SET_PAGE_ADDR = 0x22;

// Display start line (0x40-0x7F, lower 6 bits = start row)
static constexpr uint8_t CMD_SET_START_LINE_MASK = 0x3F;
static constexpr uint8_t CMD_SET_START_LINE_BASE = 0x40;

// Contrast control (1-byte parameter: 0x00-0xFF)
static constexpr uint8_t CMD_SET_CONTRAST = 0x81;

// Segment remap: col0 → SEG0
static constexpr uint8_t CMD_SEG_REMAP_0 = 0xA0;

// Segment remap: col127 → SEG0 (horizontal flip)
static constexpr uint8_t CMD_SEG_REMAP_1 = 0xA1;

// Normal display (white pixel = GDDRAM bit 1)
static constexpr uint8_t CMD_NORMAL_DISPLAY = 0xA6;

// Inverse display (white pixel = GDDRAM bit 0)
static constexpr uint8_t CMD_INVERT_DISPLAY = 0xA7;

// Multiplex ratio (1-byte parameter: ignored)
static constexpr uint8_t CMD_SET_MULTIPLEX = 0xA8;

// Display off
static constexpr uint8_t CMD_DISPLAY_OFF = 0xAE;

// Display on
static constexpr uint8_t CMD_DISPLAY_ON = 0xAF;

// Page start address (for page addressing mode, 0xB0-0xB7)
static constexpr uint8_t CMD_SET_PAGE_START_MASK = 0x07;
static constexpr uint8_t CMD_SET_PAGE_START_BASE = 0xB0;

// COM scan direction: COM0 → COM63 (forward)
static constexpr uint8_t CMD_COM_SCAN_INC = 0xC0;

// COM scan direction: COM63 → COM0 (vertical flip)
static constexpr uint8_t CMD_COM_SCAN_DEC = 0xC8;

// Display offset (1-byte parameter: ignored)
static constexpr uint8_t CMD_SET_DISPLAY_OFFSET = 0xD3;

// Clock divide ratio / oscillator frequency (1-byte parameter: ignored)
static constexpr uint8_t CMD_SET_CLK_DIV = 0xD5;

// Pre-charge period (1-byte parameter: ignored)
static constexpr uint8_t CMD_SET_PRECHARGE = 0xD9;

// COM pins hardware configuration (1-byte parameter: ignored)
static constexpr uint8_t CMD_SET_COM_PINS = 0xDA;

// Vcomh deselect level (1-byte parameter: ignored)
static constexpr uint8_t CMD_SET_VCOMH = 0xDB;

// NOP
static constexpr uint8_t CMD_NOP = 0xE3;

}  // namespace ssd1309
}  // namespace lcdtap
