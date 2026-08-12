#include "testrig/ctrl_init.hpp"

#include <initializer_list>

#include "lcdtap/devices/ili9341.hpp"
#include "lcdtap/devices/ks0108.hpp"
#include "lcdtap/devices/ssd1306.hpp"
#include "lcdtap/devices/ssd1331.hpp"
#include "lcdtap/devices/st7032.hpp"
#include "lcdtap/devices/st7789.hpp"
#include "pico/stdlib.h"
#include "testrig/bus.hpp"

namespace testrig {

using lcdtap::ControllerFamily;
using lcdtap::InterfaceFormat;

namespace {

// Command with parameters that travel as DC=1 data (ST7789/ILI9341 style).
void cmdD(uint8_t cmd, std::initializer_list<uint8_t> params) {
  busWriteCommand(cmd);
  if (params.size() > 0) {
    busWriteParams(params.begin(), params.size(), true);
  }
}

// Command with parameters that are further DC=0 command bytes
// (SSD1306/SSD1331 style).
void cmdC(uint8_t cmd, std::initializer_list<uint8_t> params) {
  busWriteCommand(cmd);
  for (uint8_t p : params) busWriteCommand(p);
}

// Realistic COLMOD parameter for an interface format (low 3 bits select the
// format on the target; the RGB-interface nibble mirrors common drivers).
uint8_t colmodByte(InterfaceFormat fmt) {
  switch (fmt) {
    case InterfaceFormat::RGB111_HPACK2_H2L_RA8: return 0x61;
    case InterfaceFormat::RGB444_HPACK2_H2L_BE: return 0x53;
    case InterfaceFormat::RGB666_UNPACK_LA8_BE: return 0x66;
    default: return 0x55;  // RGB565
  }
}

}  // namespace

void ctrlInitDisplay(ControllerFamily fam, const TestVector& vec,
                     uint16_t textRows) {
  switch (fam) {
    case ControllerFamily::ST7789:
      cmdD(lcdtap::st7789::CMD_SWRESET, {});
      sleep_ms(10);
      cmdD(lcdtap::st7789::CMD_SLPOUT, {});
      sleep_ms(10);
      cmdD(lcdtap::st7789::CMD_COLMOD, {colmodByte(vec.interfaceFormat)});
      cmdD(lcdtap::st7789::CMD_MADCTL, {0x00});
      cmdD(lcdtap::st7789::CMD_DISPON, {});
      break;

    case ControllerFamily::ILI9341:
      cmdD(lcdtap::ili9341::CMD_SWRESET, {});
      sleep_ms(10);
      cmdD(lcdtap::ili9341::CMD_SLPOUT, {});
      sleep_ms(10);
      cmdD(lcdtap::ili9341::CMD_COLMOD, {colmodByte(vec.interfaceFormat)});
      cmdD(lcdtap::ili9341::CMD_MADCTL, {0x00});
      cmdD(lcdtap::ili9341::CMD_DISPON, {});
      break;

    case ControllerFamily::SSD1306:
      cmdC(lcdtap::ssd1306::CMD_SET_MULTIPLEX,
           {static_cast<uint8_t>(vec.buffHeight - 1)});
      cmdC(lcdtap::ssd1306::CMD_SET_ADDR_MODE, {0x00});  // horizontal
      cmdC(lcdtap::ssd1306::CMD_SEG_REMAP_0, {});
      cmdC(lcdtap::ssd1306::CMD_COM_SCAN_INC, {});
      cmdC(lcdtap::ssd1306::CMD_CHARGE_PUMP, {0x14});
      cmdC(lcdtap::ssd1306::CMD_DISPLAY_ON, {});
      break;

    case ControllerFamily::SSD1331: {
      uint8_t depth = (vec.interfaceFormat == InterfaceFormat::RGB332)
                          ? lcdtap::ssd1331::REMAP_COLOR_DEPTH_256
                          : lcdtap::ssd1331::REMAP_COLOR_DEPTH_65K;
      cmdC(lcdtap::ssd1331::CMD_SETREMAP, {depth});
      cmdC(lcdtap::ssd1331::CMD_DISPLAYON, {});
      break;
    }

    case ControllerFamily::ST7032: {
      uint8_t func = lcdtap::st7032::CMD_FUNCTION_SET | lcdtap::st7032::FUNC_DL;
      if (textRows >= 2) func |= lcdtap::st7032::FUNC_N;
      busWriteCommand(func);
      busWriteCommand(lcdtap::st7032::CMD_DISPLAY_ONOFF |
                      lcdtap::st7032::DISPLAY_D);
      busWriteCommand(lcdtap::st7032::CMD_CLEAR_DISPLAY);
      sleep_ms(2);
      break;
    }

    case ControllerFamily::KS0108:
      // Both chips at once: display on, start line 0, page 0, column 0.
      busSetCs2(3);
      busWriteCommand(lcdtap::ks0108::CMD_DISPLAY_ONOFF_BASE |
                      lcdtap::ks0108::CMD_DISPLAY_ONOFF_MASK);
      busWriteCommand(lcdtap::ks0108::CMD_SET_START_LINE_BASE);
      busWriteCommand(lcdtap::ks0108::CMD_SET_PAGE_BASE);
      busWriteCommand(lcdtap::ks0108::CMD_SET_COL_BASE);
      break;

    default: break;
  }
}

void ctrlBeginFrame(ControllerFamily fam, const TestVector& vec, uint16_t x0,
                    uint16_t y0, uint16_t w, uint16_t h) {
  uint16_t x1 = static_cast<uint16_t>(x0 + w - 1);
  uint16_t y1 = static_cast<uint16_t>(y0 + h - 1);
  switch (fam) {
    case ControllerFamily::ST7789:
      cmdD(lcdtap::st7789::CMD_CASET,
           {static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0),
            static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1)});
      cmdD(lcdtap::st7789::CMD_RASET,
           {static_cast<uint8_t>(y0 >> 8), static_cast<uint8_t>(y0),
            static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1)});
      busWriteCommand(lcdtap::st7789::CMD_RAMWR);
      break;

    case ControllerFamily::ILI9341:
      cmdD(lcdtap::ili9341::CMD_CASET,
           {static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0),
            static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1)});
      cmdD(lcdtap::ili9341::CMD_RASET,
           {static_cast<uint8_t>(y0 >> 8), static_cast<uint8_t>(y0),
            static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1)});
      busWriteCommand(lcdtap::ili9341::CMD_RAMWR);
      break;

    case ControllerFamily::SSD1306:
      cmdC(lcdtap::ssd1306::CMD_SET_COL_ADDR,
           {static_cast<uint8_t>(x0), static_cast<uint8_t>(x1)});
      cmdC(lcdtap::ssd1306::CMD_SET_PAGE_ADDR,
           {static_cast<uint8_t>(y0 / 8), static_cast<uint8_t>(y1 / 8)});
      break;

    case ControllerFamily::SSD1331:
      // Write pointer resets on SETROW, so SETCOLUMN must come first.
      cmdC(lcdtap::ssd1331::CMD_SETCOLUMN,
           {static_cast<uint8_t>(x0), static_cast<uint8_t>(x1)});
      cmdC(lcdtap::ssd1331::CMD_SETROW,
           {static_cast<uint8_t>(y0), static_cast<uint8_t>(y1)});
      break;

    default: break;
  }
  (void)vec;
}

void ctrlKs0108SetPageCol(uint8_t csMask, uint8_t page, uint8_t col) {
  busSetCs2(csMask);
  busWriteCommand(lcdtap::ks0108::CMD_SET_PAGE_BASE |
                  (page & lcdtap::ks0108::CMD_SET_PAGE_MASK));
  busWriteCommand(lcdtap::ks0108::CMD_SET_COL_BASE |
                  (col & lcdtap::ks0108::CMD_SET_COL_MASK));
}

void ctrlSetTextRow(uint16_t row, uint16_t cols) {
  // HD44780 layout: rows 0/1 at 0x00/0x40, rows 2/3 continue those lines
  // at +cols (4-row fold, matches st7032_controller.cpp ddramIndexAtCell).
  uint8_t addr = static_cast<uint8_t>(((row & 1u) ? 0x40 : 0x00) +
                                      ((row >= 2) ? cols : 0));
  busWriteCommand(
      static_cast<uint8_t>(lcdtap::st7032::CMD_SET_DDRAM_ADDR | addr));
}

}  // namespace testrig
