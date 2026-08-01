#pragma once

// Debug statistics for pico2w_remote: input-path counters only (there is no
// video output). Modeled on pico2_universal's stats module.

#include <cstdint>

#include "lcdtap/lcdtap.hpp"
#include "lcdtap/pico2/i2c_slave.hpp"
#include "lcdtap/pico2/spi_slave.hpp"

struct StatsSources {
  lcdtap::LcdTap* lcdtap;
  const lcdtap::BusType* currentIface;
  lcdtap::pico2::SpiSlaveState* spi;
  lcdtap::pico2::I2cSlaveState* i2c;
};

void statsInit(const StatsSources& src);

// Call from the main loop; rate calculation and RXSTALL polling run at 1 Hz.
void statsTick(uint64_t nowMs);

int statsCollect(lcdtap::StatEntry* out, int maxCount);

void statsReset();
