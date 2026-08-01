<p align="center"><img src="./image/logo.png" width="240"></p>

A library and its example design that receives LCD controller commands (via SPI or I2C)
and outputs the framebuffer as a DVI-D signal.

## Overview

See [introduction page](https://shapoco.github.io/lcdtap/).

## Implementations

### [LcdTap-Pico2 Universal](example/pico2_universal/)

Supports multiple LCD controllers and interfaces, selectable at runtime via an OSD menu.

### [LcdTap-Pico2 for ST7789](example/pico2_st7789/)

Simplified version for 240x320 SPI LCDs with ST7789 or ILI9341 controller.

### [LcdTap-Pico2 for SSD1306](example/pico2_ssd1306/)

Simplified version 128x64 monochrome OLEDs with SSD1306 controller.

### [LcdTap-Pico2W Remote](example/pico2w_remote/)

Raspberry Pi Pico 2 W version with no video output: the captured screen is
read over WiFi through an HTTP JSON API and a built-in web UI
(`http://lcdtap.local/`). WiFi setup via the
[Remote Setup page](https://shapoco.github.io/lcdtap/remote/) (WebSerial).

## License

MIT License — see [LICENSE](LICENSE).
