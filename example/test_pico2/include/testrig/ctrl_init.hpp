#pragma once

// Controller init sequences and frame addressing, built from the lcdtap
// device command constants and sent through the bus facade.

#include <cstdint>

#include "lcdtap/config.hpp"
#include "testrig/vectors.hpp"

namespace testrig {

// Reset-state to display-on init sequence for the vector's controller.
// textRows selects the ST7032 function-set N bit (ignored otherwise).
void ctrlInitDisplay(lcdtap::ControllerFamily fam, const TestVector& vec,
                     uint16_t textRows);

// Set the write window to the given rectangle and enter the pixel data
// phase (RAMWR for ST7789/ILI9341). GRAY1 y0/h must be page aligned.
void ctrlBeginFrame(lcdtap::ControllerFamily fam, const TestVector& vec,
                    uint16_t x0, uint16_t y0, uint16_t w, uint16_t h);

// Character LCDs: set the DDRAM address to the start of a visible row.
void ctrlSetTextRow(uint16_t row, uint16_t cols);

// KS0108: select the chips of csMask (busSetCs2) and set their page and
// column address. The caller streams the page data afterwards; the cs
// selection stays in effect until the next busSetCs2().
void ctrlKs0108SetPageCol(uint8_t csMask, uint8_t page, uint8_t col);

}  // namespace testrig
