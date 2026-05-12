# Sample program for Raspberry Pi Pico2 — SSD1309

## Build instructions

```bash
cd example/pico2_ssd1309
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
make -j4
```

## Input modes

This example supports two input modes, selectable via GPIO 20 at startup.

### SPI mode (default)

SPI commands are received via a parallel conversion circuit — see
[example/pico2\_st7789/README.md](../pico2_st7789/README.md) for the full
description of the 74HC4094 + 74HC4040 wiring and the BCLK/DCX signal path.
The pin assignment is identical to the ST7789 example.

### I2C mode

Connect the SSD1309 SDA and SCL lines directly to GPIO 0 and GPIO 1 with
4.7 kΩ pull-ups to 3.3 V. The Pico 2 acts as an I2C slave at address
`0x3C` (SA0 tied low). The SSD1309 I2C control byte (first byte after the
address phase) is decoded to determine whether subsequent bytes are commands
or GDDRAM data.

## Pin assignment

| GPIO  | Direction | Function |
|-------|-----------|----------|
| 0     | IN        | I2C SDA (I2C mode only) |
| 1     | IN        | I2C SCL (I2C mode only) |
| 2     | IN        | BCLK — byte clock = SCLK÷8 (SPI mode, 74HC4040 Q3 output) |
| 3     | IN        | DCX — D/C# signal (SPI mode, direct from SPI master) |
| 4–11  | IN        | D[0..7] — parallel data (SPI mode, 74HC4094 Q1–Q8 outputs) |
| 12–19 | OUT       | DVI TMDS output (pico\_sock\_cfg, driven by PicoDVI) |
| 20    | IN        | CFG: input mode select |
| 21    | IN        | CFG: DVI output resolution select |
| 22    | IN        | RESX — hardware reset, active-low (SPI mode, pull-up on board) |
| 25    | OUT       | Onboard LED |
| 26    | IN        | CFG: output rotation bit 0 |
| 27    | IN        | CFG: output rotation bit 1 |

## Configuration GPIOs

All configuration pins are read once at startup with internal pull-downs
(default = LOW). Pull HIGH to select the alternate option.

The output rotation is re-checked every DVI frame; changes take effect on the
next frame without restarting.

| GPIO  | Name          | LOW (default)           | HIGH (alternate)           |
|-------|---------------|-------------------------|----------------------------|
| 20    | INPUT\_MODE   | SPI mode                | I2C mode                   |
| 21    | DVI\_RES      | 640×480 @ 60 Hz         | 1280×720 @ 30 Hz (reduced) |
| 26+27 | ROT           | 00 = no rotation        | 01/10/11 = see table below |

`ROT` is a 2-bit field: GPIO 27 is bit 1 (MSB) and GPIO 26 is bit 0 (LSB).

| ROT value | Effect |
|-----------|--------|
| `00`      | No rotation (default) |
| `01`      | 90° clockwise — aspect ratio swapped for FIT |
| `10`      | 180° / flip |
| `11`      | 270° clockwise — aspect ratio swapped for FIT |

The scale mode is fixed to **FIT** (aspect-ratio-preserving letterbox /
pillarbox). The SSD1309 display is 128×64 pixels.

## DVI output (PicoDVI / libdvi)

DVI output is handled by [PicoDVI](https://github.com/Wren6991/PicoDVI)
(`libdvi`).

- **Core 1** runs `dvi_scanbuf_main_16bpp()` in an infinite loop, consuming
  RGB565 scanline buffers from the `q_colour_valid` queue and serialising TMDS
  data to the DVI connector via PIO0 and DMA.
- **Core 0** (main loop) calls `inst.fillScanline()` for each line and
  pushes the filled buffer to `q_colour_valid`.

The system clock is raised to match the TMDS bit-clock requirement (252 MHz for
640×480, 319.2 MHz for 1280×720 reduced), and the voltage regulator is set to
1.20 V to support these higher frequencies.

PicoDVI uses PIO0 and claims its DMA channels inside `dvi_init()`. In SPI mode,
the parallel slave PIO program runs on PIO1 (SM0), and its DMA channel is
claimed after `dvi_init()` to avoid conflicts.
