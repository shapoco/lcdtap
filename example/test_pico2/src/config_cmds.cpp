#include "testrig/config_cmds.hpp"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"
#include "testrig/rig_config.hpp"
#include "testrig/wifi_mgr.hpp"
#include "tusb.h"

// JSON settings commands on the rig's PC-facing CDC, for the
// docs/test-pico2/ Web Serial settings page. Lines starting with '{' are
// parsed; everything else on the console (log output, typed junk) is
// ignored, and the page likewise ignores non-JSON lines (jsonOnly).

namespace testrig {

namespace {

char gLine[256];
size_t gLen = 0;

// Respond with one JSON line. Written straight to the CDC (not stdio): the
// stdio driver's CRLF translation would turn "\r\n" into "\r\r\n".
void respond(const char* json) {
  if (!tud_cdc_connected()) return;
  tud_cdc_write(json, strlen(json));
  tud_cdc_write("\r\n", 2);
  tud_cdc_write_flush();
}

void respondDoc(JsonDocument& doc) {
  char buf[384];
  size_t n = serializeJson(doc, buf, sizeof(buf) - 1);
  buf[n] = '\0';
  respond(buf);
}

void handleLine(const char* line) {
  if (line[0] != '{') return;
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return;
  const char* cmd = doc["command"];
  if (cmd == nullptr) return;

  RigConfig& cfg = rigConfig();

  if (strcmp(cmd, "hello") == 0) {
    // Deliberately NOT "welcome lcdtap": the settings page uses this to
    // tell the rig apart from an accidentally connected target.
    respond("{\"response\":\"welcome testrig\"}");
    return;
  }

  if (strcmp(cmd, "getrigconfig") == 0) {
    JsonDocument r;
    r["ssid"] = cfg.ssid;
    r["pskSet"] = (cfg.psk[0] != '\0');
    r["country"] = cfg.country;
    respondDoc(r);
    return;
  }

  if (strcmp(cmd, "setrigconfig") == 0) {
    JsonObject params = doc["params"];
    if (params.isNull()) {
      respond("{\"error\":\"missing params\"}");
      return;
    }
    if (!params["ssid"].isNull()) {
      strncpy(cfg.ssid, params["ssid"] | "", sizeof(cfg.ssid) - 1);
      cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
    }
    // psk: omitted = keep stored, "" = open network.
    if (!params["psk"].isNull()) {
      strncpy(cfg.psk, params["psk"] | "", sizeof(cfg.psk) - 1);
      cfg.psk[sizeof(cfg.psk) - 1] = '\0';
    }
    if (!params["country"].isNull()) {
      strncpy(cfg.country, params["country"] | "", sizeof(cfg.country) - 1);
      cfg.country[sizeof(cfg.country) - 1] = '\0';
    }
    rigConfigSave();
    respond("{\"response\":\"ok\"}");
    // Apply immediately when WiFi is active; no reboot needed. A country
    // change only takes effect on the next power cycle (cyw43 is already
    // initialized), which is acceptable for a one-time setting.
    wifiMgrRestart();
    return;
  }

  if (strcmp(cmd, "netstatus") == 0) {
    JsonDocument r;
    r["state"] = wifiMgrStateStr();
    char ip[20] = "0.0.0.0";
    uint32_t a = wifiMgrIp();
    if (a != 0) {
      snprintf(ip, sizeof(ip), "%u.%u.%u.%u", (unsigned)(a & 0xFF),
               (unsigned)((a >> 8) & 0xFF), (unsigned)((a >> 16) & 0xFF),
               (unsigned)(a >> 24));
    }
    r["ip"] = ip;
    r["rssi"] = wifiMgrRssi();
    r["ssid"] = cfg.ssid;
    respondDoc(r);
    return;
  }

  respond("{\"error\":\"unknown command\"}");
}

}  // namespace

void configCmdsProcess() {
  // Bounded drain of anything typed/sent on the console.
  for (int i = 0; i < 64; i++) {
    int c = getchar_timeout_us(0);
    if (c < 0) break;  // PICO_ERROR_TIMEOUT
    if (c == '\r') continue;
    if (c == '\n') {
      gLine[gLen] = '\0';
      gLen = 0;
      handleLine(gLine);
      continue;
    }
    if (gLen + 1 < sizeof(gLine)) gLine[gLen++] = static_cast<char>(c);
  }
}

}  // namespace testrig
