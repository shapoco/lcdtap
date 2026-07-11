// Configuration persistence via NVS.
//
// NVS blobs are CRC-protected by the storage layer itself, so unlike the
// pico2 flash_config only a version and size check is needed here. NVS
// writes are cache-safe on ESP32; no core-pause dance is required.
//
// Namespace/key match the Arduino Preferences library's own NVS layout
// (Preferences just wraps nvs_open/nvs_get_blob/nvs_set_blob with the same
// namespace/key strings), so a device previously flashed with the Arduino
// build keeps its saved configuration across this migration.

#include "lcdtap/m5tab5/nvs_config.hpp"

#include <nvs.h>

namespace lcdtap::m5tab5 {

static constexpr const char *NVS_NAMESPACE = "lcdtap";
static constexpr const char *NVS_KEY = "cfg";

bool loadConfig(ConfigFile *out) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t len = 0;
  bool ok = false;
  if (nvs_get_blob(handle, NVS_KEY, nullptr, &len) == ESP_OK &&
      len == sizeof(ConfigFile)) {
    ok = nvs_get_blob(handle, NVS_KEY, out, &len) == ESP_OK;
    ok = ok && out->version == CONFIG_FILE_VERSION;
  }
  nvs_close(handle);
  return ok;
}

bool saveConfig(const ConfigFile &cfg) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
  ConfigFile toSave = cfg;
  toSave.version = CONFIG_FILE_VERSION;
  bool ok = nvs_set_blob(handle, NVS_KEY, &toSave, sizeof(toSave)) == ESP_OK;
  ok = ok && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return ok;
}

}  // namespace lcdtap::m5tab5
