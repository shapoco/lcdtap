#pragma once

// Bus master facade: SPI (PIO), 8-bit parallel (PIO) and I2C (hardware i2c1)
// toward the target's LCD input port, with full tristate management so only
// the active bus drives shared pins.

#include <cstddef>
#include <cstdint>

#include "lcdtap/config.hpp"

namespace testrig {

// One-time init: RST/CS inactive outputs, everything else Hi-Z.
void busInit();

// Select and configure a bus. i2cAddr is the target's 7-bit slave address
// (I2C only). par6800 selects the 6800-style E strobe (ST7032) instead of
// the 8080 WR# strobe. Asserts CS (SPI/parallel).
bool busSelect(lcdtap::BusType type, uint32_t freqHz, uint8_t i2cAddr,
               bool par6800);

// Release the bus: CS high, bus pins Hi-Z. RST keeps its level.
void busDeselect();

// Hardware reset pulse to the target's LCD front end.
// Not available in PARALLEL_2CS mode (the RST line is the CS1 select).
void busResetPulse(uint32_t lowMs, uint32_t settleMs);

// PARALLEL_2CS only: drive the high-active chip selects (bit0 = CS1 on the
// RST line, bit1 = CS2 on the CS line). Drains the PIO first so the levels
// only change at a byte boundary. No-op on other buses.
void busSetCs2(uint8_t mask);

// One command byte (DC=0 / I2C control 0x00 framing).
void busWriteCommand(uint8_t cmd);

// Command parameter bytes. asData selects the DC level: true for
// ST7789/ILI9341 (parameters travel as DC=1 data), false for controllers
// whose parameters are further DC=0 command bytes (SSD1306/SSD1331/ST7032).
void busWriteParams(const uint8_t* params, size_t len, bool asData);

// Bulk pixel/character data (DC=1 / I2C control 0x40 framing). Uses DMA on
// SPI/parallel; blocks until the bytes are on the wire.
void busWriteData(const uint8_t* data, size_t len);

}  // namespace testrig
