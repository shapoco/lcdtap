#include "lcdtap/m5tab5/ram_pool.hpp"

#include <esp_heap_caps.h>

namespace lcdtap::m5tab5 {

void *spiDmaMemPool = nullptr;
void *frameBuffMemPool = nullptr;

esp_err_t sramPoolInit() {
  if (!spiDmaMemPool) {
    spiDmaMemPool =
        heap_caps_aligned_calloc(CACHE_ALIGN, 1, SPI_DMA_BUF_BYTES,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!spiDmaMemPool) return ESP_ERR_NO_MEM;
  }
  if (!frameBuffMemPool) {
    // Fixed pool sized for the largest buffer the OSD can configure:
    // ConfigId::BUFF_WIDTH/BUFF_HEIGHT (lib/src/config.cpp) both allow up
    // to 480, and the framebuffer is RGB565 (2 bytes/px), so 480*480*2 is
    // the true worst case -- a smaller pool here would silently overflow
    // as soon as a user picks a large enough resolution from the menu.
    // PSRAM instead of internal SRAM: an SRAM placement was tried first
    // and produced DSI underrun errors ("can't fetch data from external
    // memory fast enough") on hardware, so this pool stays in PSRAM.
    frameBuffMemPool =
        heap_caps_malloc(480 * 480 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frameBuffMemPool) return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

}  // namespace lcdtap::m5tab5
