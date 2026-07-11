#ifndef LCDTAP_M5TAB5_SRAM_POOL_HPP
#define LCDTAP_M5TAB5_SRAM_POOL_HPP

#include <cstdint>

#include <esp_err.h>

namespace lcdtap::m5tab5 {

// PARLIO receive buffer, mounted directly to the RX DMA descriptors
// (zero-copy; allocated from internal DMA-capable heap by the driver
// wrapper). The raw stream is 31.25 MB/s at the maximum 62.5 MHz SCK.
// 64 KiB (with STALE_MARGIN_BYTES=16KiB in parlio_spi_slave.cpp, leaving
// only ~48 KiB of usable slack) proved too tight in practice: real-world
// scheduling jitter around inputTask's drain cadence caused chronic
// chunk staleness/drop even with CPU well under budget. 128 KiB doubles
// the usable slack and has proven reliable for low-data-rate masters
// (e.g. SSD1306, this example's intended target).
static constexpr uint32_t SPI_DMA_BUF_BYTES = 128u * 1024u;

// Alignment for the DMA buffer. Covers both the L1 (64 B) and L2 (128 B)
// cache line sizes of the ESP32-P4 so the driver mounts it without
// stash-buffer splitting.
static constexpr uint32_t CACHE_ALIGN = 128;

extern void *frameBuffMemPool;
extern void *spiDmaMemPool;

esp_err_t sramPoolInit();

}  // namespace lcdtap::m5tab5

#endif
