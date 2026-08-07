#pragma once

// =============================================================================
// LcdTap test rig (Pico 2 W) — pin assignment and global constants.
//
// The bus port is wired straight to the pico2_universal target:
// rig GPIO N -> target GPIO N-10 (RST..DC).
// =============================================================================

#include <cstdint>

// -----------------------------------------------------------------------------
// System clock. 240 MHz serves both PIO-USB (integer divider from 48 MHz)
// and the 60 MHz PIO SPI master (2 cycles/bit, divider 2). Override with
// -DTESTRIG_SYS_CLOCK_KHZ=120000 to isolate clock-related problems (the bus
// dividers adapt; 120 MHz still reaches every supported bus rate).
// -----------------------------------------------------------------------------
#ifndef TESTRIG_SYS_CLOCK_KHZ
#define TESTRIG_SYS_CLOCK_KHZ 240000
#endif
static constexpr uint32_t SYS_CLOCK_KHZ = TESTRIG_SYS_CLOCK_KHZ;

// -----------------------------------------------------------------------------
// UI: SSD1306 OLED (I2C0)
// -----------------------------------------------------------------------------
static constexpr uint PIN_OLED_SDA = 0;
static constexpr uint PIN_OLED_SCL = 1;
static constexpr uint8_t OLED_I2C_ADDR = 0x3C;

// -----------------------------------------------------------------------------
// UI keys (all low-active, internal pull-up). Dec/Inc auto-repeat on hold.
// -----------------------------------------------------------------------------
static constexpr uint PIN_KEY_DEC = 2;
static constexpr uint PIN_KEY_INC = 3;
static constexpr uint PIN_KEY_START = 4;
static constexpr uint PIN_KEY_SELECT = 5;
static constexpr uint PIN_KEY_BACK = 6;

// -----------------------------------------------------------------------------
// USB host (Pico-PIO-USB) facing the target's USB port.
// RP2350-E9 erratum: both pins need external ~15 kOhm pull-downs.
// -----------------------------------------------------------------------------
static constexpr uint8_t PIN_USB_DP = 8;  // D- is PIN_USB_DP + 1

// -----------------------------------------------------------------------------
// Bus master port facing the target's LCD input port.
// -----------------------------------------------------------------------------
static constexpr uint PIN_TGT_RST = 10;      // target RST (active low)
static constexpr uint PIN_TGT_CS = 11;       // target CS (active low)
static constexpr uint PIN_TGT_WR_SCLK = 12;  // Par8 WR# / SPI SCLK
static constexpr uint PIN_TGT_D0 = 13;       // D0 / SPI MOSI (D0..D7 = 13..20)
static constexpr uint PIN_TGT_SPI_DC = 14;   // D1 / SPI DC
static constexpr uint PIN_TGT_I2C_SDA = 18;  // D5 / I2C SDA (i2c1)
static constexpr uint PIN_TGT_I2C_SCL = 19;  // D6 / I2C SCL (i2c1)
static constexpr uint PIN_TGT_PAR_DC = 21;   // Par8 DC

// -----------------------------------------------------------------------------
// TX buffer: largest single-shot wire frame is 320x480 RGB565 (307.2 KB).
// 320x480 RGB666 (460.8 KB) does not fit and is sent as two half frames.
// -----------------------------------------------------------------------------
static constexpr uint32_t TX_BUFFER_SIZE = 320u * 480u * 2u;
