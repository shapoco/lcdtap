#pragma once

// UI OLED (SSD1306 128x64, I2C). Text-cell oriented wrapper so the UI code
// does not depend on the graphics backend (LovyanGFX today, replaceable).
//
// Grid: 21 columns x 8 rows of 6x8 cells. Big text uses 2x scale
// (10 columns x 4 rows).

#include <cstdint>

namespace testrig {

bool oledInit();

void oledClear();

// Draw text at a 6x8 cell position. invert = white background (used for
// selection highlight / blink emphasis).
void oledText(int col, int row, const char* s, bool invert = false);

// Draw text at 2x scale; col/row are still 6x8 cell units.
void oledBigText(int col, int row, const char* s, bool invert = false);

// Push the frame to the panel.
void oledFlush();

}  // namespace testrig
