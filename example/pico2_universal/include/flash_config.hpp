#pragma once

#include <cstdint>

#include "lcdtap/lcdtap.hpp"

enum class InterfaceType : uint8_t {
  I2C = 0,
  SPI_4LINE = 1,  // default
  SPI_3LINE = 2,
  PARALLEL = 3,
};

struct ConfigFile {
  lcdtap::LcdTapConfig libConfig;
  InterfaceType interfaceType;
};

bool loadConfig(ConfigFile* out);
void saveConfig(const ConfigFile& cfg);
