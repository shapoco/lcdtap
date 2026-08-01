#pragma once

// WiFi connection manager (station mode, cyw43_arch_lwip_poll).
//
// Owns the join/retry state machine, static IP configuration, hostname and
// the mDNS responder. Everything runs on Core 0; wifiMgrProcess() must be
// called from the main loop alongside cyw43_arch_poll().

#include <cstdint>

#include "net_config.hpp"

enum class WifiState : uint8_t {
  UNCONFIGURED,  // no SSID stored
  CONNECTING,
  CONNECTED,
  FAILED,  // last join attempt failed; retrying with backoff
};

// Initialize cyw43 (also needed for the LED) and start the first join
// attempt when an SSID is configured. `cfg` must outlive the manager (the
// lwIP hostname keeps a pointer into it). Returns false when the CYW43
// itself fails to initialize.
bool wifiMgrInit(NetConfig* cfg);

// Advance the state machine: poll the link status, apply retry backoff,
// (re)start mDNS when an address is acquired.
void wifiMgrProcess(uint64_t nowMs);

WifiState wifiMgrState();
const char* wifiMgrStateStr();

// Current IPv4 address in network byte order; 0 when not connected.
uint32_t wifiMgrIp();

// Last RSSI reading in dBm (0 when unavailable).
int32_t wifiMgrRssi();

// MAC address of the station interface.
void wifiMgrMac(uint8_t mac[6]);
