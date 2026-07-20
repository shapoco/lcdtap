# LcdTap-Pico2 Universal

A universal LCD-to-DVI converter example for Raspberry Pi Pico 2. With an OSD (On-Screen Display) menu for runtime configuration.

## Features

- Supports four input interfaces selectable via OSD: I2C, 4-Line SPI, 3-Line SPI, 8-bit Parallel
- Runtime configuration via OSD menu: interface, controller type, pixel format, LCD size, inversion, R/B swap, rotation, scale mode
- Settings (including selected interface) saved to flash on Apply and restored at next boot
- DVI output: 640×480@60Hz or 1280×720@30Hz selectable via GPIO21 at boot
- Composite video output (NTSC/PAL) via a resistor-ladder DAC on otherwise-unused GPIOs, selectable from the OSD
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
the input bus leaves free. Select `Output Interface` in the OSD menu (or set
`outputInterface` over USB CDC) to switch between `DVI-D`, `NTSC` and `PAL`.

Each mode needs a different system clock, so **the device resets when the
output interface changes**. Composite output is unavailable in Parallel mode,
where every GPIO is already taken by the input bus, and the OSD item is greyed
out there.

| Mode | DAC | GPIO | Colour |
|:--|:--|:--|:--|
| 4-Line / 3-Line SPI | 7-bit R-2R | 5–11 (D0–D6) | NTSC colour, PAL monochrome |
| I2C | 3-bit weighted | 5–7 (D0–D2) | monochrome |
| Parallel | — | — | not available |

PAL colour is not implemented yet; PAL currently outputs monochrome.

### Output buffer (common to both modes)

Neither resistor network can drive 75 Ω directly: doing so would need a ladder
around 91 Ω, which exceeds the 12 mA per-pin limit and makes the GPIO's own
output impedance a large fraction of each leg. Both networks therefore feed the
same single-transistor buffer, which keeps them at ~1 mA per pin and preserves
linearity.

```
resistor network --+---- base of NPN emitter follower
                   |            |
                  R_sh         collector ---- VSYS (~5 V)
                   |            |
                  GND          emitter --+---- 75R ---+---- 220uF ---> RCA
                                         |            |
                                         47R         680pF
                                         |            |
                                        GND          GND
```

- NPN: 2N3904 / 2SC2712 or similar (fT ≥ 250 MHz)
- Collector to **VSYS (~5 V)**, not 3.3 V — the emitter reaches ~2.3 V and
  needs headroom to stay out of saturation
- 47 Ω emitter resistor, then 75 Ω in series to the output
- 680 pF from the output node to GND as a reconstruction filter
  (fc ≈ 5 MHz; tune on the bench)
- 220 µF series capacitor into the RCA jack; the receiver clamps on sync, so
  the DC offset from V<sub>BE</sub> does not matter

The 75 Ω series resistor and the receiver's 75 Ω termination halve the signal,
so a 2.3 V swing at the emitter gives **1.30 V full scale at the load**: sync
tip 0 V, blanking/black 0.286 V, peak white 1.000 V.

> [!NOTE]
> The values below are nominal. The emitter follower's output impedance costs
> a few percent of amplitude, so trim the shunt resistor (R_sh) on the bench —
> or leave it and adjust `lvlBlank` / `lvlWhite` in `composite_timing.cpp`,
> since the code-to-voltage mapping is a software table.

### 7-bit R-2R ladder (SPI modes)

```
GPIO11 -- 2.0k --+
                 |
                1.0k
                 |
GPIO10 -- 2.0k --+     (repeat for GPIO9, 8, 7, 6)
                 :
                1.0k
                 |
GPIO5  -- 2.0k --+---- 3.9k (R_sh) ---- GND
                 |
                 +---- to buffer
```

Roughly 1.65 mA per pin at full scale.

### 3-bit weighted DAC (I2C mode)

I2C occupies GPIO8/9, so only GPIO5–7 remain.

```
GPIO7 -- 3.0k --+
                |
GPIO6 -- 7.5k --+---- 4.3k (R_sh) ---- GND
                |
GPIO5 -- 15k  --+---- to buffer
```

| GPIO | Resistor | Contribution at the load |
|:--|:--|:--|
| 5 (D0) | 15 kΩ | 0.143 V |
| 6 (D1) | 7.5 kΩ | 0.287 V |
| 7 (D2) | 3.0 kΩ | 0.718 V |

The conductance ratio is **1 : 2 : 5**, not the binary 1 : 2 : 4. That is
deliberate: it places the sync tip, blanking and peak white exactly on codes,
which a binary ladder cannot do (it would put blanking 14% off, and the
receiver's clamp reads that as a black-level error).

| Code | Level | |
|:--|:--|:--|
| 000 | 0.000 V | sync tip |
| 001 | 0.143 V | |
| 010 | 0.287 V | **blanking / black** |
| 011 | 0.431 V | |
| 100 | 0.718 V | |
| 101 | 0.862 V | |
| 110 | 1.005 V | **peak white** |
| 111 | 1.149 V | |

Five picture levels between black and white. Peak current is 1.1 mA on GPIO7.

These levels are mirrored in `COMPOSITE_DAC_3BIT_GPIO5.levels[]` in
`example/pico2_common/src/composite_timing.cpp`; change them together if the
resistor values change.

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
