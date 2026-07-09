#pragma once
#include <cstdint>

namespace lcdtap::m5tab5 {

// Diagnostic bit-banged I2C master with textbook timing (~50 kHz, ample
// setup/hold margins, honors nothing but reads back ACKs). Wired to the
// slave pins with two jumpers, it provides a known-good, fully compliant
// stimulus to isolate slave-side problems from master timing quirks.

struct I2cSelftestConfig {
  int pinSda;
  int pinScl;
  uint8_t slaveAddr;  // 7-bit
};

// Configure the pins (open-drain, released).
void i2cSelftestInit(const I2cSelftestConfig &cfg);

// Send one test round: (a) the SSD1306 init sequence as a single long
// write, (b) a 32-byte recognizable data-write pattern, (c) a few
// commands as individual short writes. Logs per-transaction ACK results
// to Serial. Blocking, runs for a few ms.
void i2cSelftestRun();

}  // namespace lcdtap::m5tab5
