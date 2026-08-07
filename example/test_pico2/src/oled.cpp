#include "testrig/oled.hpp"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "testrig/config.h"

namespace testrig {

namespace {

class LgfxSsd1306 : public lgfx::LGFX_Device {
 public:
  LgfxSsd1306() {
    {
      auto cfg = bus_.config();
      cfg.i2c_port = 0;
      cfg.freq_write = 400000;
      cfg.freq_read = 400000;
      cfg.pin_sda = static_cast<int16_t>(PIN_OLED_SDA);
      cfg.pin_scl = static_cast<int16_t>(PIN_OLED_SCL);
      cfg.i2c_addr = OLED_I2C_ADDR;
      cfg.prefix_cmd = 0x00;
      cfg.prefix_data = 0x40;
      cfg.prefix_len = 1;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.panel_width = 128;
      cfg.panel_height = 64;
      panel_.config(cfg);
    }
    setPanel(&panel_);
  }

 private:
  lgfx::Bus_I2C bus_;
  lgfx::Panel_SSD1306 panel_;
};

LgfxSsd1306 gLcd;
// All drawing goes into an off-screen 1bpp canvas; oledFlush() pushes it in
// one shot, so clear-then-redraw never flickers on the panel.
lgfx::LGFX_Sprite gCanvas(&gLcd);
bool gReady = false;

void drawTextScaled(int col, int row, const char* s, bool invert, int scale) {
  if (!gReady) return;
  gCanvas.setTextSize(scale);
  if (invert) {
    gCanvas.setTextColor(TFT_BLACK, TFT_WHITE);
  } else {
    gCanvas.setTextColor(TFT_WHITE, TFT_BLACK);
  }
  gCanvas.setCursor(col * 6, row * 8);
  gCanvas.print(s);
}

}  // namespace

bool oledInit() {
  gReady = gLcd.init();
  if (!gReady) return false;
  gLcd.setRotation(2);  // module is mounted upside down
  gCanvas.setColorDepth(1);
  if (gCanvas.createSprite(gLcd.width(), gLcd.height()) == nullptr) {
    gReady = false;
    return false;
  }
  gCanvas.setFont(&fonts::Font0);  // 6x8
  gCanvas.fillScreen(TFT_BLACK);
  gCanvas.pushSprite(0, 0);
  gLcd.display();
  return true;
}

void oledClear() {
  if (!gReady) return;
  gCanvas.fillScreen(TFT_BLACK);
}

void oledText(int col, int row, const char* s, bool invert) {
  drawTextScaled(col, row, s, invert, 1);
}

void oledBigText(int col, int row, const char* s, bool invert) {
  drawTextScaled(col, row, s, invert, 2);
}

void oledFlush() {
  if (!gReady) return;
  gCanvas.pushSprite(0, 0);
  gLcd.display();
}

}  // namespace testrig
