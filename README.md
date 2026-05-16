# LcdTap

A library and example that receives LCD controller commands (via SPI or I2C)
and outputs the framebuffer as a DVI-D signal.

![](image/cover.png)

## Example Design

### for ST7789, ILI9341, etc.

See [pico2_st7789](example/pico2_st7789/README.md) for build
instructions, pin assignment, and configuration details.

### for SSD1306, SSD1309, etc.

See [pico2_ssd1306](example/pico2_ssd1306/README.md) for build
instructions, pin assignment, and configuration details.

## Download Pre-built Binary

See [releases](https://github.com/shapoco/lcdtap/releases) for pre-built UF2 binaries.

## Video

https://github.com/user-attachments/assets/6f17d5dc-84d3-4a2a-a3ea-fca37591515f

## Configuration for M5Stack CoreS3

### Connection

The M5Stack CoreS3 does not have the CS signal exposed on the connector, so one of the following solutions is required.

- Wire directly to R49 on the back of the board: Hardware modification is required, but no software changes are needed.<br>
    ![](./image/m5stack_cores3_cs.jpg)
- [Route CS signal to M-Bus](https://x.com/lovyan03/status/2055491122949165549): Requires modifying M5GFX and recompiling, but no hardware modification is needed.
- Fix CS signal to Low: No software or hardware changes required, but only applicable if the SPI bus is not used for anything other than LCD control.

The remaining signals can be obtained from the rear connector. On CoreS3, MISO is used as DCX.

|LcdTap (Pico2)|Connection|
|:--|:--|
|GND|CoreS3's GND|
|GPIO0 (CFG_CLK_MODE)|Open or GND (Normal Mode)|
|GPIO1 (RESX)|Pico2's 3V3|
|GPIO2 (SCLK)|CoreS3's SPI_SCLK|
|GPIO4 (MOSI)|CoreS3's SPI_MOSI|
|GPIO5 (DCX)|CoreS3's SPI_MISO|
|GPIO6 (CS)|(See above instructions)|
|GPIO20 (CFG_LCD_SIZE)|Pico2's 3V3 (320x240)|
|GPIO21 (CFG_DVI_RES)|Select according to your display|
|GPIO22 (CFG_SWAP_RB)|Pico2's 3V3 (swap R/B)|
|GPIO26/27 (CFG_ROT0/1)|Open or GND|
|GPIO28 (CFG_INV_POL)|Pico2's 3V3 (inverted)|

### Firmware

Use pre-built firmware `lcdtap_pico2_st7789.uf2`

## Configuration for TinyJoyPad

### Connection

|LcdTap (Pico2)|Connection|
|:--|:--|
|GND|TinyJoyPad's GND|
|GPIO8 (SDA)|TinyJoyPad's SDA|
|GPIO9 (SCL)|TinyJoyPad's SCL|
|GPIO20 (CFG_INPUT_MODE)|Open or GND| 
|GPIO21 (CFG_DVI_RES)|Select according to your display|
|GPIO26/27 (CFG_ROT0/1)|binary rotary switch or DIP-switch|

See also: [LcdTap: TinyJoyPad や Arduboy を大画面で遊ぶ](https://blog.shapoco.net/2026/0514-tinyjoypad-with-large-monitor/)

### Firmware

Use pre-built firmware `lcdtap_pico2_ssd1306.uf2`

## OSD (On-Screen Display)

The library includes an optional OSD overlay (`lcdtap/osd.hpp`) that lets users
change settings at runtime using physical buttons — no host re-flash required.

Press the **Enter** button to open the menu:

```
==== LcdTap Configuration ==============
Controller Type   : ◀ ST7789     ▶
Pixel Format      : ◀ RGB565     ▶
LCD Width         :   320            px
LCD Height        :   240            px
Inversion         :   OFF
Swap R/B          :   OFF
Output Resolution : ◀ 480p@60Hz  ▶
Output Rotation   : ◀ 0          ▶  deg
Scale Mode        : ◀ FIT        ▶
Apply             :
Cancel            :
```

See [lib/README.md](lib/README.md) for the full API reference.

## License

MIT License — see [LICENSE](LICENSE).
