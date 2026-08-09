#include "testrig/rig_config.hpp"

#include <cstring>

#include "lcdtap/pico2/flash_store.hpp"
#include "testrig/usb_host.hpp"

namespace testrig {

namespace {

constexpr uint32_t SECTORS_FROM_END = 1;

RigConfig gConfig;

static_assert(sizeof(RigConfig) + 4 <= 256,
              "RigConfig + CRC must fit one flash page");

void setDefaults(RigConfig* c) {
  memset(c, 0, sizeof(*c));
  strcpy(c->country, "JP");
  c->intf = 0;        // CDC
  c->speedClass = 2;  // Fast
}

void sanitize(RigConfig* c) {
  c->ssid[sizeof(c->ssid) - 1] = '\0';
  c->psk[sizeof(c->psk) - 1] = '\0';
  c->country[sizeof(c->country) - 1] = '\0';
  if (c->country[0] == '\0') strcpy(c->country, "JP");
  if (c->intf > 1) c->intf = 0;
  if (c->speedClass > 3) c->speedClass = 2;
}

}  // namespace

void rigConfigLoad() {
  if (!lcdtap::pico2::flashStoreLoad(SECTORS_FROM_END, &gConfig,
                                     sizeof(gConfig))) {
    setDefaults(&gConfig);
  }
  sanitize(&gConfig);
}

void rigConfigSave() {
  sanitize(&gConfig);
  // flash_range_erase/program stall XIP; core 1 (PIO-USB host) must spin in
  // SRAM with IRQs off meanwhile.
  usbHostFlashAcquire();
  lcdtap::pico2::flashStoreSave(SECTORS_FROM_END, &gConfig, sizeof(gConfig));
  usbHostFlashRelease();
}

RigConfig& rigConfig() { return gConfig; }

}  // namespace testrig
