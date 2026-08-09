#pragma once

// Rig-side WiFi station manager (reduced port of pico2w_remote wifi_mgr):
// async join from RigConfig credentials, link-status polling with retry
// backoff. No mDNS, no hostname, DHCP only. cyw43_arch_init_with_country()
// is done by main.cpp (shared with the LED bring-up); this module only
// drives the STA interface.

#include <cstdint>

namespace testrig {

enum class WifiState : uint8_t {
  OFF,           // never started (CDC mode)
  UNCONFIGURED,  // started but no SSID stored
  CONNECTING,
  CONNECTED,
  FAILED,  // waiting for the retry backoff
};

// Enable STA mode and start joining with the stored credentials. Safe to
// call repeatedly (no-op once started).
void wifiMgrStart();

// Re-join with (possibly new) stored credentials. No-op unless started.
void wifiMgrRestart();

// Pump from the main loop (alongside cyw43_arch_poll()).
void wifiMgrProcess(uint32_t nowMs);

WifiState wifiMgrState();
const char* wifiMgrStateStr();
uint32_t wifiMgrIp();  // IPv4 (network order), 0 unless CONNECTED
int32_t wifiMgrRssi();

}  // namespace testrig
