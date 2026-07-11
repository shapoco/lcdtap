#pragma once
#include <cstdint>

#include "lcdtap/config.hpp"

namespace lcdtap::m5tab5 {

struct ConfigFile {
  uint32_t version;  // bump when the LcdTapConfig layout changes
  lcdtap::LcdTapConfig libConfig;
};

static constexpr uint32_t CONFIG_FILE_VERSION = 1;

// Load the saved configuration from NVS.
// Returns false if absent or the version/size does not match.
bool loadConfig(ConfigFile *out);

// Save the configuration to NVS. Returns false on failure.
bool saveConfig(const ConfigFile &cfg);

}  // namespace lcdtap::m5tab5
