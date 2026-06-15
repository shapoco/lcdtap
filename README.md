<p align="center"><img src="./image/logo.png" width="240"></p>

A library and its example design that receives LCD controller commands (via SPI or I2C)
and outputs the framebuffer as a DVI-D signal.

![](image/cover.png)

![](image/use_cases.png)

## Features

- Receives LCD controller commands and reconstructs images into the framebuffer
- Outputs framebuffer contents as DVI-D signal (using Pico-DVI)
- Programmable screen sizes (up to 480x320 supported on Pico2)
- Command Dump: Captures and displays LCD control commands
- OSD Menu: Configure settings at runtime via on-screen display
- Serial Interface: Remote configuration and framebuffer readout over USB CDC
- Supported Controllers: ST7789, ILI9341, ILI9488, SSD1306, SSD1309, SSD1331, and some variants
- Supported Interfaces: SPI (4-line, 3-line), I2C, Parallel
- Supported Pixel Formats: Monochrome, RGB111, RGB332, RGB444, RGB565, RGB666

## Video

https://github.com/user-attachments/assets/6f17d5dc-84d3-4a2a-a3ea-fca37591515f

## Implementations

### [LcdTap-Pico2 Universal](example/pico2_universal/) (Recommended)

Supports multiple LCD controllers and interfaces, selectable at runtime via an OSD menu.

![](image/board_pico2_universal.jpg)

### [LcdTap-Pico2 for ST7789](example/pico2_st7789/)

Simplified version 240x320 or 240x320 SPI LCDs.

### [LcdTap-Pico2 for SSD1306](example/pico2_ssd1306/)

Simplified version 128x64 monochrome OLEDs with SSD1306 controller.

## Pre-built Firmware

See [Releases](https://github.com/shapoco/lcdtap/releases).

## Use Cases

Click the thumbnails to see configuration details.

[<img src="./use_cases/arduboy/thumb.jpg" width="256">](./use_cases/arduboy/)
[<img src="./use_cases/j7c/thumb.jpg" width="256">](./use_cases/j7c/)
[<img src="./use_cases/m5stack_cores3/thumb.jpg" width="256">](./use_cases/m5stack_cores3/)
[<img src="./use_cases/picosystem/thumb.jpg" width="256">](./use_cases/picosystem/)
[<img src="./use_cases/soeks01m/thumb.jpg" width="256">](./use_cases/soeks01m/)
[<img src="./use_cases/thumby/thumb.jpg" width="256">](./use_cases/thumby/)
[<img src="./use_cases/tinyjoypad/thumb.jpg" width="256">](./use_cases/tinyjoypad/)

## Recommended Header Pinout / Cable Color

![](./image/header_pinout.png)

|Pin|Parallel Mode|SPI Mode|I2C Mode|Recommended Color|
|:--:|:--:|:--:|:--:|:--:|
|1|RST|RST||Violet or Brown|
|2|CS|CS||Yellow|
|3|WEN|SCLK||White|
|4|GND|GND||Black|
|5|D1|DC||Blue|
|6|D0|MOSI||Green|
|7|3V3|3V3|3V3|Red|
|8|D2||||
|9|D5||SDA|Green|
|10|D3||||
|11|D6||SCL|White|
|12|D4||||
|13|GND||GND|Black|
|14|D7||||
|15|VSYS||VSYS|Orange|
|16|DC||||

## License

MIT License — see [LICENSE](LICENSE).
