#include "lcdtap/pico2/flash_store.hpp"

#include <cstring>

#include <hardware/flash.h>
#include <hardware/sync.h>

namespace lcdtap::pico2 {

// CRC-32/ISO-HDLC (IEEE 802.3) — table-less bitwise implementation.
static uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
  }
  return ~crc;
}

// Offset from the start of flash (not XIP base) to the store's sector.
static uint32_t storeOffset(uint32_t sectorsFromEnd) {
  return PICO_FLASH_SIZE_BYTES - sectorsFromEnd * FLASH_SECTOR_SIZE;
}

bool flashStoreLoad(uint32_t sectorsFromEnd, void* out, size_t len) {
  const uint8_t* p =
      reinterpret_cast<const uint8_t*>(XIP_BASE + storeOffset(sectorsFromEnd));
  uint32_t stored;
  memcpy(&stored, p + len, sizeof(uint32_t));
  if (crc32(p, len) != stored) return false;
  memcpy(out, p, len);
  return true;
}

void flashStoreSave(uint32_t sectorsFromEnd, const void* data, size_t len) {
  // Page buffer must be in RAM — flash is inaccessible during erase/program.
  uint8_t buf[FLASH_PAGE_SIZE] = {};
  if (len + sizeof(uint32_t) > FLASH_PAGE_SIZE) return;
  memcpy(buf, data, len);
  uint32_t crc = crc32(buf, len);
  memcpy(buf + len, &crc, sizeof(uint32_t));

  // The caller must have parked any other core in SRAM before calling here.
  // Disable this core's interrupts to block any ISR that might fetch
  // flash-resident code during the erase/program window.
  uint32_t ints = save_and_disable_interrupts();

  flash_range_erase(storeOffset(sectorsFromEnd), FLASH_SECTOR_SIZE);
  flash_range_program(storeOffset(sectorsFromEnd), buf, FLASH_PAGE_SIZE);

  restore_interrupts(ints);
}

}  // namespace lcdtap::pico2
