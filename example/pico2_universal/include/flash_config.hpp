#pragma once

#include <cstdint>

#include "lcdtap/lcdtap.hpp"

struct ConfigFile {
  lcdtap::LcdTapConfig libConfig;
};

bool loadConfig(ConfigFile* out);
void saveConfig(const ConfigFile& cfg);
