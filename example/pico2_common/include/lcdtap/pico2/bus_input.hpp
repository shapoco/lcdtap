#pragma once

// Input bus management shared by the pico2 examples: switching between the
// capture interfaces (I2C, 4-line SPI, 3-line SPI, 8-bit parallel, 8-bit
// parallel with dual chip select) and draining the active ring buffer.

#include "lcdtap/lcdtap.hpp"
#include "lcdtap/pico2/i2c_slave.hpp"
#include "lcdtap/pico2/spi_slave.hpp"

namespace lcdtap::pico2 {

// Everything busInputSwitch needs besides the live LcdTap/BusType state.
// The SPI pin and PIO assignments come pre-seeded in spi->cfg (see the
// examples' init step), the same fields spiSlaveInit reuses.
struct BusInputContext {
  SpiSlaveState* spi;
  I2cSlaveState* i2c;
  I2cSlaveConfig i2cCfg;
  uint32_t* i2cRingBuf;
  uint32_t i2cRingWords;
  uint pinParWr;        // parallel write strobe (shares the SPI SCLK pin)
  uint pinParDataBase;  // D[0]; D/C# is pinParDataBase + 8

  // PARALLEL_2CS only: raw CS1 input (high active). Shares the RST pin, so
  // the example must disable its RST GPIO IRQ while this mode is active.
  // CS2 shares spi->cfg.pinCs and the strobe (/EW) shares pinParWr.
  uint pinPar2csCs1;

  // Invert the WR# input (PARALLEL only). 6800-style hosts (ST7032 E signal)
  // latch on the falling edge; inverting the pad input lets the 8080-style
  // rising-edge PIO program capture them unchanged. Also enables pull-downs
  // on D[3:0] so a 4-bit-wired host (DB7:4 -> D[7:4]) reads zeros there.
  bool parWrInvert;
};

// Teardown the active input interface (if any) and set up `next`.
// The IRQ registrations bind to the calling core's NVIC, so call this on the
// core that services the input interrupts.
void busInputSwitch(const BusInputContext& ctx, LcdTap* inst, BusType* current,
                    bool* active, BusType next);

// Drain the active interface's ring buffer into the LcdTap instance.
void busInputProcess(const BusInputContext& ctx, BusType current);

}  // namespace lcdtap::pico2
