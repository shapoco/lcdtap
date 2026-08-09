#include "testrig/wifi_mgr.hpp"

#include <cstring>

#include "lwip/netif.h"
#include "pico/cyw43_arch.h"
#include "testrig/rig_config.hpp"

namespace testrig {

namespace {

WifiState gState = WifiState::OFF;
bool gStarted = false;
uint32_t gRetryAtMs = 0;
int gFailCount = 0;
int32_t gLastRssi = 0;
uint32_t gLastRssiMs = 0;

constexpr uint32_t RETRY_MS = 5000u;
constexpr uint32_t RETRY_SLOW_MS = 30000u;
constexpr int RETRY_SLOW_AFTER = 5;

struct netif* staNetif() { return &cyw43_state.netif[CYW43_ITF_STA]; }

void startJoin() {
  const RigConfig& cfg = rigConfig();
  cyw43_arch_wifi_connect_async(
      cfg.ssid, cfg.psk[0] != '\0' ? cfg.psk : nullptr,
      cfg.psk[0] != '\0' ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN);
  gState = WifiState::CONNECTING;
}

}  // namespace

void wifiMgrStart() {
  if (gStarted) return;
  gStarted = true;
  cyw43_arch_enable_sta_mode();
  if (rigConfig().ssid[0] == '\0') {
    gState = WifiState::UNCONFIGURED;
    return;
  }
  gFailCount = 0;
  startJoin();
}

void wifiMgrRestart() {
  if (!gStarted) return;
  if (rigConfig().ssid[0] == '\0') {
    gState = WifiState::UNCONFIGURED;
    return;
  }
  gFailCount = 0;
  startJoin();
}

void wifiMgrProcess(uint32_t nowMs) {
  if (gState == WifiState::OFF || gState == WifiState::UNCONFIGURED) return;

  const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

  switch (gState) {
    case WifiState::CONNECTING:
      if (link == CYW43_LINK_UP) {
        gState = WifiState::CONNECTED;
        gFailCount = 0;
      } else if (link < 0) {
        // CYW43_LINK_FAIL / CYW43_LINK_NONET / CYW43_LINK_BADAUTH
        gFailCount++;
        gState = WifiState::FAILED;
        gRetryAtMs =
            nowMs + (gFailCount >= RETRY_SLOW_AFTER ? RETRY_SLOW_MS : RETRY_MS);
      }
      break;

    case WifiState::CONNECTED:
      if (link != CYW43_LINK_UP) {
        startJoin();  // link lost (AP power cycle etc.) — rejoin now
      } else if (nowMs - gLastRssiMs >= 2000u) {
        // RSSI poll is a control-path exchange with the chip; keep it slow.
        gLastRssiMs = nowMs;
        int32_t rssi = 0;
        if (cyw43_wifi_get_rssi(&cyw43_state, &rssi) == 0) gLastRssi = rssi;
      }
      break;

    case WifiState::FAILED:
      if (nowMs >= gRetryAtMs) startJoin();
      break;

    default: break;
  }
}

WifiState wifiMgrState() { return gState; }

const char* wifiMgrStateStr() {
  switch (gState) {
    case WifiState::OFF: return "OFF";
    case WifiState::UNCONFIGURED: return "NO CONFIG";
    case WifiState::CONNECTING: return "CONNECTING";
    case WifiState::CONNECTED: return "CONNECTED";
    default: return "FAILED";
  }
}

uint32_t wifiMgrIp() {
  if (gState != WifiState::CONNECTED) return 0;
  return ip4_addr_get_u32(netif_ip4_addr(staNetif()));
}

int32_t wifiMgrRssi() { return gLastRssi; }

}  // namespace testrig
