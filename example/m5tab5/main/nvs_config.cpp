// Configuration persistence via NVS (Preferences).
//
// NVS blobs are CRC-protected by the storage layer itself, so unlike the
// pico2 flash_config only a version and size check is needed here. NVS
// writes are cache-safe on ESP32; no core-pause dance is required.

#include "lcdtap/m5tab5/nvs_config.hpp"

#include <Preferences.h>

namespace lcdtap::m5tab5 {

static constexpr const char *NVS_NAMESPACE = "lcdtap";
static constexpr const char *NVS_KEY = "cfg";

bool loadConfig(ConfigFile *out) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) return false;
  bool ok = false;
  if (prefs.getBytesLength(NVS_KEY) == sizeof(ConfigFile)) {
    ok = prefs.getBytes(NVS_KEY, out, sizeof(ConfigFile)) == sizeof(ConfigFile);
    ok = ok && out->version == CONFIG_FILE_VERSION;
  }
  prefs.end();
  return ok;
}

bool saveConfig(const ConfigFile &cfg) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return false;
  ConfigFile toSave = cfg;
  toSave.version = CONFIG_FILE_VERSION;
  bool ok = prefs.putBytes(NVS_KEY, &toSave, sizeof(toSave)) == sizeof(toSave);
  prefs.end();
  return ok;
}

}  // namespace lcdtap::m5tab5
