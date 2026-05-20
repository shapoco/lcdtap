#pragma once

#include <cstdint>

namespace lcdtap::ssd1331 {

// Column and row address
static constexpr uint8_t CMD_SETCOLUMN = 0x15;  // 2 params: col_start, col_end
static constexpr uint8_t CMD_SETROW = 0x75;     // 2 params: row_start, row_end

// Drawing commands
static constexpr uint8_t CMD_DRAWLINE = 0x21;  // 7 params: x0,y0,x1,y1,R,G,B
static constexpr uint8_t CMD_DRAWRECT =
    0x22;  // 10 params: x0,y0,x1,y1,Rb,Gb,Bb,Rf,Gf,Bf
static constexpr uint8_t CMD_COPY = 0x23;  // 6 params: x0,y0,x1,y1,x_dst,y_dst
static constexpr uint8_t CMD_DIMWINDOW = 0x24;    // 4 params: x0,y0,x1,y1
static constexpr uint8_t CMD_CLEARWINDOW = 0x25;  // 4 params: x0,y0,x1,y1
static constexpr uint8_t CMD_FILLENABLE = 0x26;   // 1 param: bit0=fill_en

// Contrast (ignored)
static constexpr uint8_t CMD_SETCONTRASTCCA = 0x81;
static constexpr uint8_t CMD_SETCONTRASTCCB = 0x82;
static constexpr uint8_t CMD_SETCONTRASTCCC = 0x83;
static constexpr uint8_t CMD_MASTERCURRENT = 0x87;

// Display control
static constexpr uint8_t CMD_SETREMAP = 0xA0;   // 1 param: remap & color depth
static constexpr uint8_t CMD_STARTLINE = 0xA1;  // 1 param (ignored)
static constexpr uint8_t CMD_DISPLAYOFFSET = 0xA2;  // 1 param (ignored)
static constexpr uint8_t CMD_NORMALDISPLAY = 0xA6;
static constexpr uint8_t CMD_INVERTDISPLAY = 0xA7;
static constexpr uint8_t CMD_DISPLAYALLON = 0xA4;
static constexpr uint8_t CMD_DISPLAYALLOFF = 0xA5;
static constexpr uint8_t CMD_MULTIPLEX = 0xA8;  // 1 param (ignored)
static constexpr uint8_t CMD_SETMASTER = 0xAD;  // 1 param (ignored)
static constexpr uint8_t CMD_DISPLAYOFF = 0xAE;
static constexpr uint8_t CMD_DISPLAYON = 0xAF;

// Timing and power (ignored)
static constexpr uint8_t CMD_POWERMODE = 0xB0;
static constexpr uint8_t CMD_PRECHARGE = 0xB1;
static constexpr uint8_t CMD_CLOCKDIV = 0xB3;
static constexpr uint8_t CMD_PRECHARGEA = 0xBB;
static constexpr uint8_t CMD_PRECHARGEB = 0xBC;
static constexpr uint8_t CMD_PRECHARGEC = 0xBD;
static constexpr uint8_t CMD_VCOMH = 0xBE;

// SETREMAP bit masks
static constexpr uint8_t REMAP_ADDR_INC = 1u << 0;   // 0=horiz, 1=vert
static constexpr uint8_t REMAP_COL_REMAP = 1u << 1;  // 1=reverse column order
static constexpr uint8_t REMAP_BGR = 1u << 2;        // 1=BGR color order
static constexpr uint8_t REMAP_COM_REMAP = 1u << 5;  // 1=COM scan from bottom
static constexpr uint8_t REMAP_COLOR_DEPTH_MASK = 0x03u << 6;
static constexpr uint8_t REMAP_COLOR_DEPTH_256 = 0x00u << 6;  // 8bpp RGB332
static constexpr uint8_t REMAP_COLOR_DEPTH_65K = 0x01u << 6;  // 16bpp RGB565

}  // namespace lcdtap::ssd1331
