#pragma once

// Rig settings persisted in the last flash sector (pico2_common
// flash_store): WiFi credentials for the rig itself plus the title-screen
// selections. The target's IP is NOT stored — it is queried over CDC
// (netstatus) at test time.

#include <cstdint>

namespace testrig {

struct RigConfig {
  char ssid[33];       // "" = WiFi unconfigured
  char psk[64];        // "" = open network
  char country[3];     // 2-letter country code for cyw43 init, e.g. "JP"
  uint8_t intf;        // title screen: 0 = CDC, 1 = WiFi
  uint8_t speedClass;  // title screen: SpeedClass index (0..3)
  uint8_t reserved[24];
};

// Load from flash into the live config (defaults when the CRC fails).
void rigConfigLoad();

// Persist the live config. Parks core 1 around the flash write; call only
// while no test run is active.
void rigConfigSave();

// The live config.
RigConfig& rigConfig();

}  // namespace testrig
