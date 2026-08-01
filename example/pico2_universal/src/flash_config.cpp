#include "flash_config.hpp"

#include <hardware/flash.h>

#include "lcdtap/pico2/flash_store.hpp"

static_assert(sizeof(ConfigFile) + sizeof(uint32_t) <= FLASH_PAGE_SIZE,
              "ConfigFile too large for one flash page");

// The device config lives in the last flash sector.
static constexpr uint32_t SECTORS_FROM_END = 1u;

bool loadConfig(ConfigFile *out) {
  return lcdtap::pico2::flashStoreLoad(SECTORS_FROM_END, out,
                                       sizeof(ConfigFile));
}

// Caller must have paused Core 1 via the video backend's flashAcquire()
// before calling here, so Core 1 is spinning in SRAM and cannot access flash
// during erase/program.
void saveConfig(const ConfigFile &cfg) {
  lcdtap::pico2::flashStoreSave(SECTORS_FROM_END, &cfg, sizeof(ConfigFile));
}
