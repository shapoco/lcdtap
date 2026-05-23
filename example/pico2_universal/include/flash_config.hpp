#pragma once

#include <cstdint>

#include "lcdtap/lcdtap.hpp"

enum class InterfaceType : uint8_t {
  I2C = 0,
  SPI_4LINE = 1,  // default
  SPI_3LINE = 2,
  PARALLEL = 3,
};

enum class BootMode : uint8_t {
  DVI_OUTPUT = 0,
  USB_MASS_STORAGE = 1,
};

struct ConfigFile {
  lcdtap::LcdTapConfig libConfig;
  InterfaceType interfaceType;
  BootMode bootMode;
};

bool loadConfig(ConfigFile* out);
void saveConfig(const ConfigFile& cfg);
