#include "wifi_mgr.hpp"

#include <cstring>

#include "pico/cyw43_arch.h"

#include "lwip/apps/mdns.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"

static NetConfig* gCfg = nullptr;
static WifiState gState = WifiState::UNCONFIGURED;
static uint64_t gRetryAtMs = 0;
static int gFailCount = 0;
static int32_t gLastRssi = 0;
static uint64_t gLastRssiMs = 0;
static bool gMdnsRunning = false;
static bool gHadAddress = false;

static constexpr uint32_t RETRY_MS = 5000u;
static constexpr uint32_t RETRY_SLOW_MS = 30000u;
static constexpr int RETRY_SLOW_AFTER = 5;

static struct netif* staNetif() { return &cyw43_state.netif[CYW43_ITF_STA]; }

static uint32_t countryCode(const NetConfig& cfg) {
  if (cfg.country[0] == '\0' || cfg.country[1] == '\0') {
    return CYW43_COUNTRY_WORLDWIDE;
  }
  return CYW43_COUNTRY(cfg.country[0], cfg.country[1], 0);
}

// Apply hostname / static addressing on top of the driver's defaults. The
// cyw43 glue re-adds the netif (and starts DHCP) every time the STA
// interface comes up, so this runs after every join start.
static void applyNetifConfig() {
  struct netif* n = staNetif();
  netif_set_hostname(n, gCfg->hostname);
  if (!gCfg->useDhcp && gCfg->staticIp != 0) {
    dhcp_stop(n);
    ip4_addr_t ip, mask, gw;
    ip.addr = gCfg->staticIp;
    mask.addr = gCfg->netmask;
    gw.addr = gCfg->gateway;
    netif_set_addr(n, &ip, &mask, &gw);
    if (gCfg->dns != 0) {
      ip_addr_t dns;
      ip_addr_set_ip4_u32(&dns, gCfg->dns);
      dns_setserver(0, &dns);
    }
  }
}

static void startJoin() {
  cyw43_arch_wifi_connect_async(
      gCfg->ssid, gCfg->psk[0] != '\0' ? gCfg->psk : nullptr,
      gCfg->psk[0] != '\0' ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN);
  applyNetifConfig();
  gState = WifiState::CONNECTING;
}

static void mdnsStart() {
  if (!gMdnsRunning) {
    mdns_resp_init();
    mdns_resp_add_netif(staNetif(), gCfg->hostname);
    mdns_resp_add_service(staNetif(), gCfg->hostname, "_http", DNSSD_PROTO_TCP,
                          80, nullptr, nullptr);
    gMdnsRunning = true;
  } else {
    mdns_resp_restart(staNetif());
  }
}

bool wifiMgrInit(NetConfig* cfg) {
  gCfg = cfg;
  if (cyw43_arch_init_with_country(countryCode(*cfg)) != 0) {
    return false;
  }
  cyw43_arch_enable_sta_mode();

  if (!netConfigValid(*cfg)) {
    gState = WifiState::UNCONFIGURED;
    return true;
  }
  startJoin();
  return true;
}

void wifiMgrProcess(uint64_t nowMs) {
  if (gState == WifiState::UNCONFIGURED) return;

  const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

  switch (gState) {
    case WifiState::CONNECTING:
      if (link == CYW43_LINK_UP) {
        gState = WifiState::CONNECTED;
        gFailCount = 0;
        gHadAddress = true;
        mdnsStart();
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
        // Link lost (AP power cycle etc.) — rejoin immediately.
        gState = WifiState::CONNECTING;
        startJoin();
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
    case WifiState::UNCONFIGURED: return "UNCONFIGURED";
    case WifiState::CONNECTING: return "CONNECTING";
    case WifiState::CONNECTED: return "CONNECTED";
    case WifiState::FAILED: return "FAILED";
  }
  return "?";
}

uint32_t wifiMgrIp() {
  if (gState != WifiState::CONNECTED) return 0;
  return ip4_addr_get_u32(netif_ip4_addr(staNetif()));
}

int32_t wifiMgrRssi() { return gLastRssi; }

void wifiMgrMac(uint8_t mac[6]) { memcpy(mac, cyw43_state.mac, 6); }
