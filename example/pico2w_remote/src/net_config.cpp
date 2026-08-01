#include "net_config.hpp"

#include <cstring>

#include <hardware/flash.h>

#include "lcdtap/pico2/flash_store.hpp"

#include "config.h"

static_assert(sizeof(NetConfig) + sizeof(uint32_t) <= FLASH_PAGE_SIZE,
              "NetConfig too large for one flash page");

void netConfigDefault(NetConfig* out) {
  memset(out, 0, sizeof(*out));
  out->useDhcp = 1u;
  strcpy(out->country, "JP");
  strcpy(out->hostname, "lcdtap");
}

bool netConfigLoad(NetConfig* out) {
  if (lcdtap::pico2::flashStoreLoad(NET_CONFIG_SECTORS_FROM_END, out,
                                    sizeof(NetConfig))) {
    // A blob from flash may predate the terminator conventions; force them.
    out->ssid[sizeof(out->ssid) - 1] = '\0';
    out->psk[sizeof(out->psk) - 1] = '\0';
    out->country[sizeof(out->country) - 1] = '\0';
    out->hostname[sizeof(out->hostname) - 1] = '\0';
    if (out->hostname[0] == '\0') strcpy(out->hostname, "lcdtap");
    return true;
  }
  netConfigDefault(out);
  return false;
}

bool netConfigValid(const NetConfig& cfg) { return cfg.ssid[0] != '\0'; }
