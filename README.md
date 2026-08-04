<p align="center"><img src="./image/logo.png" width="240"></p>

A library and its example design that receives LCD controller commands (via SPI or I2C)
and outputs the framebuffer as a DVI-D signal.

## Overview

See [introduction page](https://shapoco.github.io/lcdtap/).

## Implementations

### [LcdTap-Pico2 Universal](example/pico2_universal/)

Supports multiple LCD controllers (ST7789, ILI9341, SSD1306, SSD1331, ST7032)
and interfaces, selectable at runtime via an OSD menu.

### [LcdTap-Pico2W Remote](example/pico2w_remote/)

Raspberry Pi Pico 2 W version with no video output: the captured screen is
read over WiFi through an HTTP JSON API and a built-in web UI
(`http://lcdtap.local/`). WiFi setup via the
[Remote Setup page](https://shapoco.github.io/lcdtap/remote/) (WebSerial).

## License

MIT License — see [LICENSE](LICENSE).
