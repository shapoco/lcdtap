#pragma once

// Network configuration persisted in its own flash sector (separate from the
// LcdTap device config so either can be rewritten without touching the
// other).

#include <cstdint>

struct NetConfig {
  char ssid[33];      // empty = WiFi unconfigured
  char psk[64];       // empty = open network
  uint8_t useDhcp;    // 0 = static addressing below
  char country[3];    // ISO 3166-1 alpha-2; empty = worldwide
  uint32_t staticIp;  // all four in network byte order (ip4_addr u32)
  uint32_t netmask;
  uint32_t gateway;
  uint32_t dns;
  char hostname[32];  // also the mDNS name (<hostname>.local)
  uint8_t reserved[24];
};

// Fill in the defaults: unconfigured SSID, DHCP, hostname "lcdtap",
// country "JP".
void netConfigDefault(NetConfig* out);

// Load from flash; falls back to defaults (returns false) when absent or
// written by a different layout. Saving goes through main.cpp's
// flash-parked helper, so there is no save function here.
bool netConfigLoad(NetConfig* out);

// True when an SSID has been configured.
bool netConfigValid(const NetConfig& cfg);
