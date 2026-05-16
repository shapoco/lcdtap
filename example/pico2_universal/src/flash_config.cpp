#include "flash_config.hpp"

#include <cstring>

#include "hardware/flash.h"
#include "hardware/sync.h"

static_assert(sizeof(lcdtap::LcdTapConfig) + sizeof(uint32_t) <=
                  FLASH_PAGE_SIZE,
              "LcdTapConfig too large for one flash page");

// Offset from the start of flash (not XIP base) to the last sector.
static constexpr uint32_t kFlashOffset =
    PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;

// XIP address for reading back the stored data.
static constexpr uintptr_t kFlashXipAddr = XIP_BASE + kFlashOffset;

// CRC-32/ISO-HDLC (IEEE 802.3) — table-less bitwise implementation.
static uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
  }
  return ~crc;
}

bool loadConfig(lcdtap::LcdTapConfig *out) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(kFlashXipAddr);
  uint32_t stored;
  memcpy(&stored, p + sizeof(lcdtap::LcdTapConfig), sizeof(uint32_t));
  if (crc32(p, sizeof(lcdtap::LcdTapConfig)) != stored) return false;
  memcpy(out, p, sizeof(lcdtap::LcdTapConfig));
  return true;
}

void saveConfig(const lcdtap::LcdTapConfig &cfg) {
  // Page buffer must be in RAM — flash is inaccessible during erase/program.
  uint8_t buf[FLASH_PAGE_SIZE] = {};
  memcpy(buf, &cfg, sizeof(cfg));
  uint32_t crc = crc32(buf, sizeof(cfg));
  memcpy(buf + sizeof(cfg), &crc, sizeof(uint32_t));

  // Core 1 runs only SRAM-resident code (PicoDVI and Pico SDK queue/semaphore
  // functions are all __not_in_flash_func / __time_critical_func), so no
  // lockout is needed. Disable Core 0 interrupts only to prevent any ISR from
  // fetching code via XIP during the flash operation.
  uint32_t ints = save_and_disable_interrupts();

  flash_range_erase(kFlashOffset, FLASH_SECTOR_SIZE);
  flash_range_program(kFlashOffset, buf, FLASH_PAGE_SIZE);

  restore_interrupts(ints);
}
