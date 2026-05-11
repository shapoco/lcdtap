# Sample program for Raspberry Pi Pico2

## Build instructions

```bash
cd example/pico2
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
make -j4
```

## Pin assignment

| GPIO | Direction | Function |
|------|-----------|----------|
| 2    | IN        | BCLK — byte clock = SCLK÷8 (74HC4040 Q3 output) |
| 3    | IN        | DCX — D/C# signal (direct from SPI master) |
| 4–11 | IN       | D[0..7] — parallel data (74HC4094 Q1–Q8 outputs) |
| 12–19 | OUT     | DVI TMDS output (pico\_sock\_cfg, driven by PicoDVI) |
| 20   | IN        | CFG: LCD size select |
| 21   | IN        | CFG: DVI output resolution select |
| 22   | IN        | RESX — hardware reset, active-low (pull-up on board) |
| 25   | OUT       | Onboard LED |
| 26   | IN        | CFG: scale mode bit 0 |
| 27   | IN        | CFG: scale mode bit 1 |
| 28   | IN        | CFG: inversion polarity |

The SPI signal path uses external ICs to convert the serial SPI bus into a
parallel byte interface:

- **74HC4094** (serial-in / parallel-out shift register): captures MOSI bits
  on each SCLK edge and presents them as D[0..7].
- **74HC4040** (12-stage ripple counter): divides SCLK by 8, generating BCLK —
  one pulse per complete byte. CS (active-high) holds the counter in reset while
  the SPI master is idle, keeping BCLK low.

## Configuration GPIOs

All configuration pins are read once at startup with internal pull-downs
(default = LOW). Pull HIGH to select the alternate option.

| GPIO | Name         | LOW (default)           | HIGH (alternate)               |
|------|--------------|-------------------------|--------------------------------|
| 20   | LCD\_SIZE    | 240×240                 | 240×320                        |
| 21   | DVI\_RES     | 640×480 @ 60 Hz         | 1280×720 @ 30 Hz (reduced)     |
| 26+27 | SCALE\_MODE | 00 = STRETCH            | 01 = FIT, 10 = PIXEL\_PERFECT  |
| 28   | INV\_POL     | INVON → inverted        | INVON → normal (polarity flip) |

`SCALE_MODE` is a 2-bit field: GPIO 27 is bit 1 (MSB) and GPIO 26 is bit 0
(LSB).  Values: `00` = STRETCH, `01` = FIT, `10` = PIXEL_PERFECT.

`INV_POL` controls how the ST7789 INVON/INVOFF commands are interpreted.
The default (LOW) matches the ST7789 datasheet polarity.

## DVI output (PicoDVI / libdvi)

DVI output is handled by [PicoDVI](https://github.com/Wren6991/PicoDVI)
(`libdvi`).

- **Core 1** runs `dvi_scanbuf_main_16bpp()` in an infinite loop, consuming
  RGB565 scanline buffers from the `q_colour_valid` queue and serialising TMDS
  data to the DVI connector via PIO0 and DMA.
- **Core 0** (main loop) calls `sl2dInst.fillScanline()` for each line and
  pushes the filled buffer to `q_colour_valid`.

The system clock is raised to match the TMDS bit-clock requirement (252 MHz for
640×480, 319.2 MHz for 1280×720 reduced), and the voltage regulator is set to
1.20 V to support these higher frequencies.

PicoDVI uses PIO0 and claims its DMA channels inside `dvi_init()`. The parallel
slave PIO program runs on PIO1 (SM0), and its DMA channel is claimed after
`dvi_init()` to avoid conflicts.
