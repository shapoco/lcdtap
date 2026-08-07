# LcdTap Test Rig (test_pico2)

A Raspberry Pi Pico 2 W based semi-automated hardware test rig for the
LcdTap `pico2_universal` target. The rig plays the role of the LCD host:
it configures the target over its USB CDC JSON protocol, drives LCD
controller command streams into the target's SPI / I2C / 8-bit parallel
input as a bus master, reads the resulting framebuffer (or text buffer)
back over CDC, and verifies it against the transmitted pattern.

DVI-D / composite / DisplayLink outputs are checked visually and are out
of scope for automatic verification.

```
 PC ── USB ──┐                     ┌──────────────┐
             │  ┌──────────────┐   │ pico2_universal
   (console, │  │  Test rig    │───│  (target)
    Phase 2: ├──│  Pico 2 W    │   │
    UF2 pass │  │              │───│ LCD bus in
    through) │  └──────────────┘   └──────────────┘
             │     │        │
             │   OLED    keys x5
```

## Roles and phases

- **Phase 1 (this firmware)**: test execution. USB CDC host to the
  target (JSON protocol), bus master TX, streaming verification, OLED UI.
  Target BOOTSEL (MSC) presence is *detected* and shown as
  "Download Mode".
- **Phase 2 (planned)**: USB MSC passthrough — while the target is in
  BOOTSEL, the rig exposes the RPI-RP2 drive to the PC and forwards UF2
  writes, ported from the proven `mimicusb` project (core0 device stack
  with `TUD_MSC_RET_BUSY` deferred IO, core1 host stack, cross-core IO
  queue, media insert/remove via SCSI sense NOT READY). The device
  descriptors already carry the optional MSC interface behind
  `-DTESTRIG_MSC_PASSTHROUGH=1`.

## Hardware

### MCU / board

Raspberry Pi Pico 2 W (RP2350). The onboard LED (CYW43 WL_GPIO0) is a
heartbeat.

### Pin assignment

The bus port is wired straight to the target: **rig GPIO N → target
GPIO N−10**.

| Rig GPIO | Function | Target GPIO |
|---:|---|---:|
| 0 | UI OLED SDA (I2C0) | — |
| 1 | UI OLED SCL (I2C0) | — |
| 2 | Dec key (low active, pull-up, auto-repeat) | — |
| 3 | Inc key (low active, pull-up, auto-repeat) | — |
| 4 | Start key (low active, pull-up) | — |
| 5 | Select key (low active, pull-up) | — |
| 6 | Back key (low active, pull-up) | — |
| 8 | USB host D+ (Pico-PIO-USB) | target USB D+ |
| 9 | USB host D− | target USB D− |
| 10 | RST (active low) | 0 |
| 11 | CS (active low) | 1 |
| 12 | WR# / SCLK | 2 |
| 13 | D0 / SPI MOSI | 3 |
| 14 | D1 / SPI DC | 4 |
| 15–17 | D2–D4 | 5–7 |
| 18 | D5 / I2C SDA (i2c1) | 8 |
| 19 | D6 / I2C SCL (i2c1) | 9 |
| 20 | D7 | 10 |
| 21 | Parallel DC | 11 |

Only the pins of the active bus are driven; everything else on GPIO11–21
is kept high-impedance.

### External components (required)

- **GPIO8/9: ~15 kΩ pull-downs to GND.** RP2350 erratum E9 makes the
  internal pull-downs unreliable as USB host bus-reset terminations.
- SSD1306 128x64 I2C OLED on GPIO0/1 (address 0x3C), plus I2C pull-ups
  if the module has none.
- I2C pull-ups on the target bus lines (rig GPIO18/19) are recommended
  for the 1 MHz / 2 MHz settings; both sides also enable internal
  pull-ups.
- The rig must supply VBUS to the target's USB port.

## Clocking and on-chip resources

- `clk_sys` = **240 MHz** (multiple of 12 MHz for PIO-USB) with
  `VREG_VOLTAGE_1_15`.
- The SPI spec point of 62.5 MHz is intentionally replaced by
  **60 MHz** (240 MHz / 2 cycles-per-bit / divider 2): 62.5 MHz would
  require a 125/250 MHz clock, which PIO-USB cannot run from.
- PIO allocation: pio0 = PIO-USB TX, pio1 = PIO-USB RX, pio2 SM0 = bus
  master (SPI or parallel program, loaded on demand), CYW43 LED SPI
  claims a remaining SM (the init order in `main.cpp` steers it to
  pio2).
- Core 0: device USB (PC console), UI, executor. Core 1: TinyUSB host
  stack over PIO-USB (mount callbacks, CDC FIFO pumps). The cores
  exchange CDC bytes through lock-free SPSC rings.
- TX pattern buffer: one static 307.2 KB buffer (`TX_BUFFER_SIZE`,
  320×480 RGB565). Larger wire frames (320×480 RGB666 = 460.8 KB) are
  generated and sent in row chunks inside a single RAMWR.

## Bus master details

| Bus | Implementation | Rates |
|---|---|---|
| SPI mode 0 | PIO (pio2), 2 cycles/bit, MSB first; DC on GPIO14 switched between command/data phases after a TXSTALL drain | 10 / 20 / 40 / 60 MHz |
| 8-bit parallel | PIO, 3 cycles/byte: data setup with WR# low, target latches on the rising edge, 2-cycle hold (the target samples D+DC ~26 ns after the edge) | 1 / 2.5 / 5 / 10 MHz |
| I2C | hardware i2c1; control-byte framing per the target's parser (first byte per transaction: bit6 = D/C, Co=0 streams the rest) | 100 k / 400 k / 1 M / 2 MHz |

- For ST7032 targets the parallel strobe is inverted with
  `gpio_set_outover()`: the inverted 8080 WR# waveform is exactly the
  6800 E timing the target expects (`parWrInvert` on the target side).
- 2 MHz I2C is beyond the FM+ spec; it relies on the DW I2C timing
  registers accepting the divider. Validate on hardware; the vector can
  be treated as expected-fail if the silicon disagrees.
- CS is asserted for the whole test sequence and released at the end
  (the CS rising edge resets the target's capture state machine).

## Test vectors

31 built-in vectors plus the **ALL** entry (runs every vector, continues
on failure). Defaults use the "Fast" clock (third choice of each bus:
I2C 1 MHz, SPI 40 MHz, parallel 5 MHz). See `src/vectors.cpp` for the
full table: SSD1306 (I2C rot 0–3, SPI), SSD1331 (RGB332 rot 0–3,
RGB565, RGB666-RA), ST7789 (RGB444 rot 0–3, RGB565, RGB666-LA),
ILI9341, ILI9488 (RGB111 rot 0–3, RGB565, RGB666-LA, trim CUSTOM/AUTO,
parallel), and ST7032 text presets (8x2/16x2/16x4/20x4 I2C, 20x4
parallel).

Customization (per selected vector, volatile): bus interface, clock,
resolution, pixel format, rotation, trim (Off / Custom / Auto with
x=11, y=15, w=width/2, h=height/2). Choice lists are constrained per
controller family.

Formats that the controller command set cannot express are forced via
the target's `intfFmtOvr` setting: SSD1331 RGB666-RA (SETREMAP encodes
only RGB332/RGB565) and ILI9341-family RGB111 (COLMOD 0x01 unmapped).
Everything else is selected through real COLMOD / SETREMAP commands.

## Test procedure (per vector)

1. `getparams` for the vector's base preset; capture `swapRB`,
   `i2cAddr`, `textCols/Rows`.
2. Echo all preset values back via `setparams` with the vector's
   overrides (`busInterface`, `buffWidth/Height`, `trimMode/X/Y/W/H`,
   `outputRot`, `intfFmtOvr`, `flipMode=0`, `forcePwrOn=0`,
   `outputInterface=0`) plus **`"save": false`** so flash is not worn by
   automated runs. Keeping `outputInterface` at DVI-D avoids target
   reboots.
3. Wait 300 ms, `statsreset`.
4. Select the bus, pulse RST (10 ms low, 120 ms settle), send the
   controller init sequence (SWRESET/SLPOUT/COLMOD/MADCTL/DISPON etc.).
5. Send 10 solid-color dummy frames (one row built once, resent per
   line), then immediately one PRNG compare frame.
6. `getframebuffer {"writeProtected": true}` — the Base64/RLE stream is
   decoded and compared on the fly (below). Character LCD vectors send
   dummy text then per-row pattern text and compare via
   `gettextbuffer` (exact character codes).
7. `getstats`: the deltas of `RX Drop`, `RX HW Overflow` and
   `Unknown Commands` must be zero.

Pass = pixel/text compare AND clean stats. The result screen reports
the failed stage, mismatch count and first bad coordinate.

### Trim vectors

Auto trim on the target expands from the *addressed window*
(CASET/RASET), not from pixel content. AUTO vectors therefore address
only the trim rectangle (window, dummy and compare frames all confined
to it) and expect the readback region to equal it exactly. CUSTOM
vectors send the full frame; the target reports only the trim region.

## Verification engine

- **Pattern**: a counter-based hash PRNG (`hash32(seed ^ physIndex)`)
  gives O(1) random access to any pixel's canonical 8-bit RGB channels.
  Wire encoders emit the top N bits per channel for the vector's
  format; the expected RGB565 value derives from the same channels, so
  no expected-frame buffer exists anywhere.
- **Streaming compare**: the `getframebuffer` payload is fed character
  by character through Base64 → RGB565-RLE → pixel decode. The target
  emits the trim region in post-rotation order (`fbIndexTrimmed`); the
  verifier applies the same mapping back to physical coordinates,
  recomputes the expected hash and compares under a per-format
  significant-bit mask (e.g. RGB444 compares the top 4 bits of each
  channel), which removes any dependency on the target's low-bit
  expansion policy. `swapRB` swaps the compared fields and mask widths.
- Display inversion needs no special handling: the target XORs pixels
  on write and `getframebuffer` XORs them again with the same flag.
- Peak readback is ~410 KB of Base64 at Full-Speed USB (~1 s); nothing
  is buffered.

## UI

128x64 OLED (LovyanGFX, 6x8 font) and five tact switches (all
low-active with internal pull-ups, 1 ms sampling, level accepted after
3 identical samples). The Dec/Inc keys auto-repeat while held: first
repeat after 500 ms, then every 100 ms.

- **Vector select** (boot): Inc/Dec scroll ALL / #1..#31; the title
  blinks. Start runs, Select customizes (not on ALL).
- **Item select**: Inc/Dec pick Intf/Freq/Reso/Fmt/Rot/Trim (text
  vectors: Intf/Freq only); the item name blinks. Select edits, Back
  returns.
- **Value select**: Inc/Dec change the candidate; the value blinks.
  Select applies, Back cancels.
- **Running**: vector name + percent; Back cancels (between steps).
- **Result**: big OK/NG; on NG the failed list scrolls with Inc/Dec
  and the detail line shows stage / mismatch count / first bad pixel. Select
  jumps to customization of the selected failure, Back to vector
  select.
- **Download Mode**: while the target enumerates as MSC, the OLED
  blinks "Download Mode" and inputs are ignored; the previous screen is
  restored on exit.

The PC-facing CDC console logs pass/fail lines via `printf`
(`pico_stdio_usb` cannot coexist with `tinyusb_host`, so a minimal
custom stdio driver feeds the device CDC).

## JSON protocol usage

See `example/pico2_common/UART_PROTOCOL.md`. The rig added the
`"save": false` option to `setparams` (apply without persisting) —
without it, every `setparams` costs a flash sector erase and a Core 1
pause on the target. The transport is abstracted (`Transport` in
`include/testrig/link.hpp`: `sendLine`/`recvChar`); a future
pico2w_remote HTTP transport (`POST /api`, same JSON) can replace the
CDC implementation without touching the executor.

## Build

```sh
git submodule update --init submodule/Pico-PIO-USB \
    submodule/ArduinoJson submodule/LovyanGFX
cd example/test_pico2
PICO_SDK_PATH=... ./build.sh      # PICO_BOARD=pico2_w
```

Host-side unit tests (no hardware; the closed loop runs the real lcdtap
library and the target's own RLE/Base64 encoders):

```sh
cd example/test_pico2/test_host
# build commands are in the header comments of:
#   testgen_test.cpp  — wire encoders vs reference decoders
#   verify_test.cpp   — closed loop: testgen -> LcdTap -> serialize -> verify
```

The closed-loop test found and pinned lib issue 0013 (RGB666-LA red LSB
leaking into green bit 10) before any hardware existed.

## Known limitations / future work

- Phase 2 MSC passthrough not yet enabled (`TESTRIG_MSC_PASSTHROUGH`).
- Target parallel input is marked untested in pico2_universal — expect
  to shake out target-side issues; start at 1 MHz.
- Rotation vectors validate the config path, input decode and the
  rotated readback mapping; the DVI-side rendering itself remains a
  visual check.
- pico2w_remote support: implement `Transport` over WiFi HTTP.
