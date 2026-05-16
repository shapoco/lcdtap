# pico2_universal

A universal LCD-to-DVI converter example for Raspberry Pi Pico 2. Based on `pico2_st7789`, with an OSD (On-Screen Display) menu for runtime configuration. Hardware jumpers for most settings are replaced by a 5-button keypad.

## Features

- Supports four input interfaces selectable via OSD: I2C, 4-Line SPI, 3-Line SPI, Parallel
- Runtime configuration via OSD menu: interface, controller type, pixel format, LCD size, inversion, R/B swap, rotation, scale mode
- Settings (including selected interface) saved to flash on Apply and restored at next boot
- DVI output: 640×480@60Hz or 1280×720@30Hz selectable via GPIO21 at boot

## OSD Menu

Press the **Enter key** (GPIO28) to open the configuration menu.

| Key | Action |
|-----|--------|
| Up / Down | Navigate menu items |
| Left / Right | Adjust selected value |
| Enter on Apply | Apply changes and save to flash |
| Enter on Cancel | Discard changes and close |

### Interface Selection

The **Interface** item (second in the menu) selects the input interface:

| Value | Interface | Description |
|-------|-----------|-------------|
| I2C | I2C slave | I2C0 on GPIO8 (SDA) / GPIO9 (SCL), address 0x3C |
| 4Line SPI | 4-line SPI (default) | MOSI + separate DCX pin |
| 3Line SPI | 3-line SPI | DCX embedded as first bit of each 9-bit transaction |
| Parallel | Parallel 8-bit | External 74HC595 + 74HC4040 + 74AHC1G04 circuit |

The selected interface takes effect immediately on Apply (no reboot required).

## GPIO Assignments

### Common (all modes)

| GPIO | Direction | Function |
|------|-----------|----------|
| 1 | IN | RESX — hardware reset, active-low (pull-up) |
| 12–19 | OUT | DVI-D TMDS output (PicoDVI pico_sock_cfg) |
| 20 | IN | KEY_UP — active-low (pull-up) |
| 21 | IN | CFG: DVI_RES — LOW=640×480@60Hz, HIGH=1280×720@30Hz (pull-down) |
| 22 | IN | KEY_DOWN — active-low (pull-up) |
| 25 | OUT | Onboard LED |
| 26 | IN | KEY_LEFT — active-low (pull-up) |
| 27 | IN | KEY_RIGHT — active-low (pull-up) |
| 28 | IN | KEY_ENTER — active-low (pull-up) |

### I2C Mode

| GPIO | Direction | Function |
|------|-----------|----------|
| 8 | IN | I2C SDA (pull-up) |
| 9 | IN | I2C SCL (pull-up) |

### 4-Line SPI Mode (default)

| GPIO | Direction | Function |
|------|-----------|----------|
| 2 | IN | SPI SCLK |
| 4 | IN | SPI MOSI — MSB first |
| 5 | IN | SPI DCX — D/C# signal |
| 6 | IN | SPI CS — chip select, active-low (pull-up) |

### 3-Line SPI Mode

| GPIO | Direction | Function |
|------|-----------|----------|
| 2 | IN | SPI SCLK |
| 4 | IN | SPI MOSI — DCX as first bit, then D7..D0 |
| 6 | IN | SPI CS — chip select, active-low (pull-up) |

### Parallel Mode

| GPIO | Direction | Function |
|------|-----------|----------|
| 2 | IN | PAR BCLK — byte clock (SCLK÷8) |
| 3 | IN | PAR DCX — D/C# signal |
| 4–11 | IN | PAR D[0..7] — parallel data (GPIO8/9 shared with I2C) |

> **Note:** GPIO8 and GPIO9 are shared between I2C mode (SDA/SCL) and Parallel mode (PAR\_DATA[4:5]). Wire only the interface actually in use.

## Build

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
cmake .. -DCMAKE_BUILD_TYPE=Release -DPICO_COPY_TO_RAM=1 -DPICO_BOARD=pico2 \
         -DLCDTAP_LCD_SIZE_W=320 -DLCDTAP_LCD_SIZE_H=240
```

## Video Output

DVI-D via HDMI connector (PicoDVI `pico_sock_cfg`, GPIO12–19).

- GPIO21 LOW (default): 640×480@60Hz (252 MHz system clock)
- GPIO21 HIGH: 1280×720@30Hz reduced (319.2 MHz system clock)
