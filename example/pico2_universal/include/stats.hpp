#pragma once

// Debug statistics aggregator.
//
// Collects counters from the LcdTap library, the input drivers (SPI/I2C) and
// the active video backend into a StatEntry array shared by the OSD
// statistics view and the UART "getstats" command. Manual reset is
// implemented by baseline subtraction so no IRQ- or Core-1-owned counter is
// ever written from here (see statsReset()).

#include <cstdint>

#include "lcdtap/lcdtap.hpp"
#include "lcdtap/pico2/composite_out.hpp"
#include "lcdtap/pico2/hstx_out.hpp"
#include "lcdtap/pico2/i2c_slave.hpp"
#include "lcdtap/pico2/spi_slave.hpp"

#include "displaylink_out.hpp"
#include "output_interface.hpp"

struct StatsSources {
  lcdtap::LcdTap* lcdtap;
  const lcdtap::BusType* currentIface;  // live; changes on interface switch
  lcdtap::pico2::SpiSlaveState* spi;
  lcdtap::pico2::I2cSlaveState* i2c;
  lcdtap::pico2::HstxOutState* hstx;
  lcdtap::pico2::CompositeOutState* cvbs;
  DisplaylinkOutState* displaylink;
  OutputInterface outputIf;  // fixed at boot (changes require a reboot)
};

void statsInit(const StatsSources& src);

// Call from the Core 0 main loop on the per-frame tick. Updates the RX rate
// once per second and polls the PIO RXSTALL sticky flag.
void statsTick(uint64_t nowMs);

// Fill out[] with up to maxCount entries; returns the number written.
int statsCollect(lcdtap::StatEntry* out, int maxCount);

// Snapshot all counters as the new baseline ("reset" as seen by displays).
void statsReset();
