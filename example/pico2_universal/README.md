# LcdTap-Pico2 Universal

A universal LCD-to-DVI converter example for Raspberry Pi Pico 2. With an OSD (On-Screen Display) menu for runtime configuration.

## Features

- Supports four input interfaces selectable via OSD: I2C, 4-Line SPI, 3-Line SPI, 8-bit Parallel
- Runtime configuration via OSD menu: interface, controller type, pixel format, LCD size, inversion, R/B swap, rotation, scale mode
- Settings (including selected interface) saved to flash on Apply and restored at next boot
- DVI output: 640×480@60Hz or 1280×720@30Hz selectable via GPIO21 at boot
- Composite video output (NTSC/PAL) on otherwise-unused GPIOs, selectable from the OSD: a 1-pin PWM DAC or a 7-bit R-2R ladder
- USB CDC serial interface for remote configuration and framebuffer readout from a PC or smartphone

> [!WARNING]
> Parallel Mode has not been tested yet. Please let me know if it works!

## Schematics

- Keep the input bus as short as possible and ensure all cables are the same length.
- See also: [Recommended Header Pinout](../../README.md#recommended-header-pinout)

![](./image/schematics.png)

## GPIO Assignments

### Common (all modes)

| GPIO  | Direction | Name | Active-low | Internal Pull-up | Description |
|:--:|:--:|:--|:--:|:--:|:--|
| 0     | IN        | RST | v | v | LCD Hardware reset (SPI mode) |
| 5–11  | OUT       | CVBS_D[0..6] | | | Composite R-2R ladder (SPI modes only, when selected) |
| 10    | OUT       | CVBS_PWM | | | Composite PWM output (SPI/I2C, when selected) |
| 12–19 | OUT       | (DVI signals) | | | RP2350 HSTX |
| 20    | IN        | CFG_OUT_720P | v | v | High=640×480@60Hz,<br>Low=1280×720@30Hz |
| 21    | IN        | KEY_DOWN | v | v | Low=pressed |
| 22    | IN        | KEY_LEFT | v | v | Low=pressed |
| 26    | IN        | KEY_UP | v | v | Low=pressed |
| 27    | IN        | KEY_RIGHT | v | v | Low=pressed |
| 28    | IN        | KEY_ENTER | v | v | Low=pressed |

### I2C Mode

| GPIO  | Direction | Name | Active-low | Internal Pull-up | Description |
|:--:|:--:|:--|:--:|:--:|:--|
| 8     | IN        | SDA | | v | I2C data |
| 9     | IN        | SCL | | v | I2C clock |

### 4-Line/3-Line SPI Mode

| GPIO  | Direction | Name | Active-low | Internal Pull-up | Description |
|:--:|:--:|:--|:--:|:--:|:--|
| 1     | IN        | CS | v | v | LCD Chip select |
| 2     | IN        | SCLK | | | SPI clock from master |
| 3     | IN        | MOSI | | | SPI data from master |
| 4     | IN        | DC | | | D/C# signal from master (4-line mode only) |

### Parallel Mode

| GPIO  | Direction | Name | Active-low | Internal Pull-up | Description |
|:--:|:--:|:--|:--:|:--:|:--|
| 1 | IN | CS | v | v | LCD Chip select|
| 2 | IN | WR | | | Write strobe |
| 3–10 | IN | D[0..7] | | | parallel data |
| 11 | IN | DC | | | D/C# signal |

## Composite Video Output (NTSC/PAL)

The HDMI connector occupies GPIO12–19, so composite video uses the GPIOs that
the input bus leaves free. Two independent OSD items control it (or
`outputInterface` / `compositeDac` over USB CDC):

- **`Output`** — `DVI-D`, `NTSC` or `PAL`
- **`Composite DAC`** — `PWM` or `R-2R`

There are therefore two quality tiers, and you only build the one you want:

| DAC | GPIO | Parts | Picture | Buses |
|:--|:--|:--|:--|:--|
| **PWM** | 10 | 1 resistor + 1 capacitor | ~12 luma steps, ~70% amplitude | SPI, I2C |
| **R-2R** | 5–11 | 8 resistors + transistor | 70 luma steps, full amplitude | SPI only |

The R-2R ladder spans GPIO5–11, which covers the I2C pins (GPIO8/9), so it is
selectable only on SPI; the OSD greys it out otherwise. Composite output is
unavailable on the parallel bus entirely — GPIO3–10 are data lines there, which
covers the PWM pin too.

**The device resets** when `Output` changes (each mode needs a different system
clock), and when the bus or the DAC changes while a composite mode is running.

PAL colour is not implemented yet; PAL currently outputs monochrome on both
DACs.

> [!NOTE]
> Both DACs share GPIO10, so the two circuits are alternative populations, not
> simultaneous ones. Build whichever tier you want.

### PWM DAC — simple tier

One GPIO, one resistor, one capacitor. No buffer.

```
GPIO10 --- R ---+--- RCA centre
                |
                C
                |
               GND
```

The PWM period equals the sample period, so the duty resolution *is* the
sample period in system clocks: 22 steps on NTSC, 17 on PAL. White sits at
duty 17/22 rather than 100%, because chroma overshoots white by up to
+133 IRE and has to fit above it.

That, plus the 12 mA per-pin limit, is why the amplitude falls short of the
1 V standard — a single 3.3 V GPIO simply cannot reach it:

| R | White level | Sync amplitude | Peak current |
|:--|:--|:--|:--|
| 120 Ω | 0.78 V (78%) | 0.23 V (80%) | 13.5 mA |
| **150 Ω** | **0.70 V (70%)** | **0.21 V (72%)** | **12.0 mA** |
| 180 Ω | 0.63 V (63%) | 0.18 V (64%) | 10.8 mA |

Start at **R = 150 Ω** and drop to 120 Ω if a receiver refuses to lock.
Most TVs slice sync on the AC-coupled signal and tolerate this; capture
devices are less forgiving. C = 100–470 pF (corner ≈ 5–6 MHz).

The PWM carrier sits at 14.3 MHz with chroma at 3.58 MHz — only two octaves
apart — so a single-pole RC leaves visible carrier ripple. That is the
accepted cost of this tier.

### R-2R ladder — quality tier

```
GPIO11 -- 2.0k --+
                 |
                1.0k
                 |
GPIO10 -- 2.0k --+     (repeat for GPIO9, 8, 7, 6)
                 :
                1.0k
                 |
GPIO5  -- 2.0k --+---- 3.9k ---- GND
                 |
                 +---- base of NPN emitter follower
```

The ladder cannot drive 75 Ω directly: doing so would need R ≈ 91 Ω, which
exceeds the 12 mA per-pin limit and makes the GPIO's own output impedance a
large fraction of each leg. A single-transistor buffer keeps it at 1.65 mA per
pin and preserves linearity.

```
ladder output ---- base of NPN emitter follower
                          |
                    collector ---- VSYS (~5 V)
                          |
                     emitter --+---- 75R ---+---- 220uF ---> RCA
                               |            |
                               47R         680pF
                               |            |
                              GND          GND
```

- NPN: 2N3904 / 2SC2712 or similar (fT ≥ 250 MHz)
- Collector to **VSYS (~5 V)**, not 3.3 V — the emitter reaches ~2.3 V and
  needs headroom to stay out of saturation
- 220 µF blocks DC at the jack; the receiver clamps on sync, so the
  V<sub>BE</sub> offset does not matter

Full scale (code 127) is 1.30 V at a 75 Ω terminated load: sync tip 0 V,
blanking/black 0.286 V, peak white 1.000 V.

> [!NOTE]
> All values are nominal. The emitter follower costs a few percent of
> amplitude, so trim the 3.9 kΩ shunt on the bench — or leave it and adjust
> `lvlBlank` / `lvlWhite` in `composite_timing.cpp`, since the code-to-voltage
> mapping is a software table.

## Uploading Firmware

1. Download zip file from [releases](https://github.com/shapoco/lcdtap/releases).
2. Extract `lcdtap_pico2_universal.uf2` from the zip file.
3. Connect the Pico 2 to your computer while holding the BOOTSEL button to enter bootloader mode.
4. Copy the UF2 file to the mounted drive.

## OSD Menu

Press the Enter key to open the configuration menu.

![](./image/osd_ss.png)

| Key | Action |
|-----|--------|
| Up / Down | Navigate menu items |
| Left / Right | Adjust selected value |
| Enter on Apply | Apply changes and save to flash |
| Enter on Cancel | Discard changes and close |
| Enter on Command Dump | Open the command dump viewer |

### Command Dump Viewer

Selecting **Command Dump** opens a hex viewer that shows the raw LCD command/data stream captured since power-on (or since the last trigger).

![](./image/cmd_dump_ss.png)

| Key | Action |
|-----|--------|
| Enter | Clear buffer and start a new capture |
| Right | Stop capture (mark complete) |
| Left | Return to the configuration menu |
| Up / Down | Scroll |

Each row displays 16 entries. Colors indicate the type of each entry:

| Color | Meaning |
|-------|---------|
| Black on cyan | Command byte |
| White on black / dark-gray (alternating) | Data byte |
| Black on yellow (`HR`) | Hardware reset event |
| `..` in dark gray | Empty (not yet captured) |

## USB Serial Interface

LcdTap-Pico2 Universal accepts JSON commands over the USB CDC serial interface.
This allows remote configuration and framebuffer readout from a PC.

Available here: https://shapoco.github.io/lcdtap/monitor

![](./image/web_app.png)

## Building Firmware from Source

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
./build.sh
```

The UF2 file is generated at `build/lcdtap_pico2_universal.uf2`.

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `LCDTAP_LCD_SIZE_W` | 240 | Initial LCD framebuffer width (px) |
| `LCDTAP_LCD_SIZE_H` | 320 | Initial LCD framebuffer height (px) |

Example:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 \
         -DLCDTAP_LCD_SIZE_W=320 -DLCDTAP_LCD_SIZE_H=240
```

## External Deserializer for High-Speed SPI

The SPI interface of LcdTap-Pico2 Universal can support clock frequencies up to approximately 50MHz. For frequencies exceeding this, you can add an external deserializer outside the Pico2 to support higher speeds. In this case, select Parallel as the Interface in the OSD menu.

![](./image/des_schematics.png)

## Troubleshooting

### Failed to boot normally

Try powering on or resetting while pressing the Left key. This will boot while ignoring the settings saved in flash.
