# pico2_universal

A universal LCD-to-DVI converter example for Raspberry Pi Pico 2. Based on `pico2_st7789`, with an OSD (On-Screen Display) menu for runtime configuration. Hardware jumpers for most settings are replaced by a 5-button keypad.

## Features

- Supports Normal Mode (direct SPI) and Fast Mode (74HC595/74HC4040 parallel) via GPIO0
- Runtime configuration via OSD menu: controller type, pixel format, LCD size, inversion, R/B swap, rotation, scale mode
- DVI output: 640×480@60Hz or 1280×720@30Hz selectable via GPIO21 at boot

## OSD Menu

Press the **Enter key** (GPIO28) to open the configuration menu.

| Key | Action |
|-----|--------|
| Up / Down | Navigate menu items |
| Left / Right | Adjust selected value |
| Enter on Apply | Apply changes |
| Enter on Cancel | Discard changes and close |

## GPIO Assignments

### Normal Mode (default)

| GPIO | Direction | Function |
|------|-----------|----------|
| 0 | IN | CFG: CLK_MODE — LOW=Normal Mode, HIGH=Fast Mode (pull-down) |
| 1 | IN | RESX — hardware reset, active-low (pull-up) |
| 2 | IN | SPI SCLK |
| 4 | IN | SPI MOSI — MSB first |
| 5 | IN | SPI DCX — D/C# signal |
| 6 | IN | SPI CS — chip select, active-low (pull-up) |
| 12–19 | OUT | DVI-D TMDS output (PicoDVI pico_sock_cfg) |
| 20 | IN | KEY_UP — active-low (pull-up) |
| 21 | IN | CFG: DVI_RES — LOW=640×480@60Hz, HIGH=1280×720@30Hz (pull-down) |
| 22 | IN | KEY_DOWN — active-low (pull-up) |
| 25 | OUT | Onboard LED |
| 26 | IN | KEY_LEFT — active-low (pull-up) |
| 27 | IN | KEY_RIGHT — active-low (pull-up) |
| 28 | IN | KEY_ENTER — active-low (pull-up) |

### Fast Mode (GPIO0 = HIGH)

| GPIO | Direction | Function |
|------|-----------|----------|
| 0 | IN | CFG: CLK_MODE — HIGH=Fast Mode (pull-down) |
| 1 | IN | RESX — hardware reset, active-low (pull-up) |
| 2 | IN | PAR BCLK — byte clock (SCLK÷8) |
| 3 | IN | PAR DCX — D/C# signal |
| 4–11 | IN | PAR D[0..7] — parallel data |
| 12–19 | OUT | DVI-D TMDS output |
| 20 | IN | KEY_UP — active-low (pull-up) |
| 21 | IN | CFG: DVI_RES (pull-down) |
| 22 | IN | KEY_DOWN — active-low (pull-up) |
| 25 | OUT | Onboard LED |
| 26 | IN | KEY_LEFT — active-low (pull-up) |
| 27 | IN | KEY_RIGHT — active-low (pull-up) |
| 28 | IN | KEY_ENTER — active-low (pull-up) |

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
