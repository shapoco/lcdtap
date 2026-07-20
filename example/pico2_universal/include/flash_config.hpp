#pragma once

#include <cstdint>

#include "lcdtap/lcdtap.hpp"

// Growing this struct changes sizeof(ConfigFile), so configs written by an
// older build fail the CRC check in loadConfig() and are discarded. That is
// intentional and accepted: the device falls back to defaults.
struct ConfigFile {
  lcdtap::LcdTapConfig libConfig;
  uint8_t outputInterface;  // OutputInterface
  uint8_t reserved[3];      // keep 4-byte granularity, room to grow
};

bool loadConfig(ConfigFile* out);
void saveConfig(const ConfigFile& cfg);
