# Sample program for Raspberry Pi Pico2 — ILI9342 (3-line SPI)

> [!WARNING]
> This is **untested reference code**. It has not been verified on real
> hardware and may contain bugs. Use it as a starting point only.

## Build instructions

```bash
cd example/pico2_3line_spi
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
make -j4
```

To override the framebuffer size, pass optional `-D` flags to `cmake`:

```bash
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk \
         -DLCDTAP_LCD_SIZE_W=240 -DLCDTAP_LCD_SIZE_H=320 \
         -DLCDTAP_LCD_SIZE_ALT_W=320 -DLCDTAP_LCD_SIZE_ALT_H=240
```

| cmake option | Default | Description |
|---|---|---|
| `LCDTAP_LCD_SIZE_W` | `240` | Framebuffer width when GPIO 20 (LCD\_SIZE) = LOW |
| `LCDTAP_LCD_SIZE_H` | `320` | Framebuffer height when GPIO 20 (LCD\_SIZE) = LOW |
| `LCDTAP_LCD_SIZE_ALT_W` | `320` | Framebuffer width when GPIO 20 (LCD\_SIZE) = HIGH |
| `LCDTAP_LCD_SIZE_ALT_H` | `240` | Framebuffer height when GPIO 20 (LCD\_SIZE) = HIGH |

## Video output

DVI signal generation uses Luke Wren's excellent library [PicoDVI](https://github.com/Wren6991/PicoDVI), and signal output uses his [Pico-DVI-Sock](https://github.com/Wren6991/Pico-DVI-Sock).

## 3-line serial interface

This program implements the ILI9342 **3-line serial protocol**.  Each
transaction is 9 bits transmitted on a single MOSI line:

- **Bit 8 (first SCLK edge):** D/C# flag — 0 = command, 1 = data
- **Bits 7..0 (next 8 SCLK edges):** data byte, MSB first

No separate D/C# pin is required.  The maximum supported SPI clock is
approximately **84 MHz** at the 640×480 system clock (252 MHz).

| GPIO  | Direction | Function |
|-------|-----------|----------|
| 1     | IN        | RESX — hardware reset, active-low (pull-up on board) |
| 2     | IN        | SCLK — SPI clock (CPOL=0: idles LOW) |
| 4     | IN        | MOSI — 9-bit serial data (D/C# first, then D7..D0) |
| 6     | IN        | CS — chip select, active-low (pull-up on board) |
| 12–19 | OUT       | DVI TMDS output (pico\_sock\_cfg, driven by PicoDVI) |
| 20    | IN        | CFG: LCD size select |
| 21    | IN        | CFG: DVI output resolution select |
| 22    | IN        | CFG: SWAP\_RB — R/B channel swap |
| 25    | OUT       | Onboard LED |
| 26    | IN        | CFG: output rotation bit 0 |
| 27    | IN        | CFG: output rotation bit 1 |
| 28    | IN        | CFG: inversion polarity |

## Configuration GPIOs

All configuration pins are read once at startup with internal pull-downs
(default = LOW). Pull HIGH to select the alternate option.

| GPIO  | Name      | LOW (default)           | HIGH (alternate)               |
|-------|-----------|-------------------------|--------------------------------|
| 20    | LCD\_SIZE | 240×320 (default, overridable at build time) | 320×240 (default, overridable at build time) |
| 21    | DVI\_RES  | 640×480 @ 60 Hz         | 1280×720 @ 30 Hz (reduced)     |
| 22    | SWAP\_RB  | no R/B swap (default)   | swap R and B channels          |
| 26+27 | ROT       | 00 = no rotation        | 01/10/11 = see table below     |
| 28    | INV\_POL  | INVON → inverted        | INVON → normal (polarity flip) |

`ROT` is a 2-bit field: GPIO 27 is bit 1 (MSB) and GPIO 26 is bit 0 (LSB).
The output rotation is re-checked every DVI frame; changes take effect on the
next frame without restarting.

| ROT value | Effect |
|-----------|--------|
| `00`      | No rotation (default) |
| `01`      | 90° clockwise — aspect ratio swapped for FIT / PIXEL\_PERFECT |
| `10`      | 180° / flip |
| `11`      | 270° clockwise — aspect ratio swapped for FIT / PIXEL\_PERFECT |

The scale mode is fixed to **FIT** (aspect-ratio-preserving letterbox / pillarbox).

`INV_POL` controls how the INVON/INVOFF commands are interpreted.
The default (LOW) matches the standard polarity.

`SWAP_RB` overrides the R/B channel swap regardless of the MADCTL BGR bit.
Pull HIGH to swap R and B channels; useful when the display panel wiring inverts the colour order.
