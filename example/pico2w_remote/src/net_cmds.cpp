#include "net_cmds.hpp"

#include <cstdio>
#include <cstring>

#include "lwip/ip4_addr.h"

#include "wifi_mgr.hpp"

static NetConfig* gLive = nullptr;
static NetConfigSaveFn gSave = nullptr;

void netCmdsInit(NetConfig* liveCfg, NetConfigSaveFn saveFn) {
  gLive = liveCfg;
  gSave = saveFn;
}

// Escape '"' and '\\' for embedding in a JSON string. Control characters are
// replaced with '?'; the lexer never produces them unescaped anyway.
static void jsonEscape(const char* in, char* out, int cap) {
  int o = 0;
  for (const char* p = in; *p != '\0' && o < cap - 2; ++p) {
    char c = *p;
    if (c == '"' || c == '\\') {
      if (o >= cap - 3) break;
      out[o++] = '\\';
      out[o++] = c;
    } else if (static_cast<uint8_t>(c) < 0x20u) {
      out[o++] = '?';
    } else {
      out[o++] = c;
    }
  }
  out[o] = '\0';
}

static void ipToStr(uint32_t ip, char* buf, int cap) {
  ip4_addr_t a;
  a.addr = ip;
  ip4addr_ntoa_r(&a, buf, cap);
}

static bool parseIp(const char* s, uint32_t* out) {
  ip4_addr_t a;
  if (!ip4addr_aton(s, &a)) return false;
  *out = ip4_addr_get_u32(&a);
  return true;
}

// Bounded string copy; returns false when the value does not fit.
static bool copyStr(char* dst, size_t cap, const char* src) {
  if (strlen(src) >= cap) return false;
  strcpy(dst, src);
  return true;
}

// ----- getnetconfig ---------------------------------------------------------
// The PSK is write-only: only its presence is reported.
static void execGetNetConfig(JsonIntf* ji) {
  char ssidEsc[70], hostEsc[70];
  jsonEscape(gLive->ssid, ssidEsc, sizeof(ssidEsc));
  jsonEscape(gLive->hostname, hostEsc, sizeof(hostEsc));
  char ip[16], mask[16], gw[16], dns[16];
  ipToStr(gLive->staticIp, ip, sizeof(ip));
  ipToStr(gLive->netmask, mask, sizeof(mask));
  ipToStr(gLive->gateway, gw, sizeof(gw));
  ipToStr(gLive->dns, dns, sizeof(dns));

  char resp[420];
  snprintf(resp, sizeof(resp),
           "{\"ssid\":\"%s\",\"pskSet\":%s,\"dhcp\":%s,"
           "\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\","
           "\"dns\":\"%s\",\"hostname\":\"%s\",\"country\":\"%s\"}\r\n",
           ssidEsc, gLive->psk[0] != '\0' ? "true" : "false",
           gLive->useDhcp ? "true" : "false", ip, mask, gw, dns, hostEsc,
           gLive->country);
  jsonIntfRespondShort(ji, resp);
}

// ----- setnetconfig ---------------------------------------------------------
// Partial update: omitted keys keep their current value. Applying requires a
// reboot (mirroring the outputInterface policy in pico2_universal); the ok
// response is flushed first, then the main loop reboots.
static void execSetNetConfig(JsonIntf* ji, const JsonParser& p) {
  if (p.paramOverflow || p.strPoolOverflow) {
    jsonIntfRespondShort(ji, "{\"error\":\"too many params\"}\r\n");
    return;
  }

  NetConfig cfg = *gLive;
  for (int i = 0; i < p.numParams; i++) {
    const char* k = p.params[i].key;
    const char* sv = jsonParamStr(p, i);
    int32_t iv = p.params[i].value;
    bool ok = true;

    if (strcmp(k, "ssid") == 0 && sv) {
      ok = copyStr(cfg.ssid, sizeof(cfg.ssid), sv);
    } else if (strcmp(k, "psk") == 0 && sv) {
      // An empty string clears the PSK (open network); to keep the stored
      // PSK, omit the key entirely.
      ok = copyStr(cfg.psk, sizeof(cfg.psk), sv);
    } else if (strcmp(k, "hostname") == 0 && sv) {
      ok = sv[0] != '\0' && copyStr(cfg.hostname, sizeof(cfg.hostname), sv);
    } else if (strcmp(k, "country") == 0 && sv) {
      ok = strlen(sv) == 2 || sv[0] == '\0';
      if (ok) copyStr(cfg.country, sizeof(cfg.country), sv);
    } else if (strcmp(k, "dhcp") == 0 && !sv) {
      cfg.useDhcp = iv != 0 ? 1u : 0u;
    } else if (strcmp(k, "ip") == 0 && sv) {
      ok = parseIp(sv, &cfg.staticIp);
    } else if (strcmp(k, "netmask") == 0 && sv) {
      ok = parseIp(sv, &cfg.netmask);
    } else if (strcmp(k, "gateway") == 0 && sv) {
      ok = parseIp(sv, &cfg.gateway);
    } else if (strcmp(k, "dns") == 0 && sv) {
      ok = parseIp(sv, &cfg.dns);
    } else {
      char resp[128];
      snprintf(resp, sizeof(resp), "{\"error\":\"bad key: %.32s\"}\r\n", k);
      jsonIntfRespondShort(ji, resp);
      return;
    }

    if (!ok) {
      char resp[128];
      snprintf(resp, sizeof(resp), "{\"error\":\"bad value for %.32s\"}\r\n",
               k);
      jsonIntfRespondShort(ji, resp);
      return;
    }
  }

  *gLive = cfg;
  gSave(cfg);
  jsonIntfRespondShort(ji, "{\"response\":\"ok\"}\r\n");
  // The WiFi stack applies the new settings from scratch on reboot; the
  // response drains first, then the main loop reboots.
  jsonIntfSetRebootPending(ji);
}

// ----- netstatus ------------------------------------------------------------
static void execNetStatus(JsonIntf* ji) {
  char ssidEsc[70], hostEsc[70];
  jsonEscape(gLive->ssid, ssidEsc, sizeof(ssidEsc));
  jsonEscape(gLive->hostname, hostEsc, sizeof(hostEsc));
  char ip[16];
  ipToStr(wifiMgrIp(), ip, sizeof(ip));
  uint8_t mac[6];
  wifiMgrMac(mac);

  char resp[300];
  snprintf(resp, sizeof(resp),
           "{\"state\":\"%s\",\"ip\":\"%s\",\"rssi\":%ld,\"ssid\":\"%s\","
           "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
           "\"hostname\":\"%s\"}\r\n",
           wifiMgrStateStr(), ip, static_cast<long>(wifiMgrRssi()), ssidEsc,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], hostEsc);
  jsonIntfRespondShort(ji, resp);
}

bool netCmdsExec(JsonIntf* ji, const JsonParser& p, void* /*ctx*/) {
  if (strcmp(p.command, "getnetconfig") == 0) {
    execGetNetConfig(ji);
    return true;
  }
  if (strcmp(p.command, "setnetconfig") == 0) {
    execSetNetConfig(ji, p);
    return true;
  }
  if (strcmp(p.command, "netstatus") == 0) {
    execNetStatus(ji);
    return true;
  }
  return false;
}
