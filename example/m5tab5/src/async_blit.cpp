#include "lcdtap/m5tab5/async_blit.hpp"

#include <esp_async_memcpy.h>
#include <esp_cache.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "lgfx/v1/platforms/esp32p4/Panel_DSI.hpp"

namespace lcdtap::m5tab5 {

namespace {
constexpr const char *TAG = "async_blit";

// This app only ever runs one AsyncBlitState with at most 2 transactions
// in flight (matching DisplayOutState's 2 ping-pong strip buffers), so a
// fixed 2-slot table -- indexed by a counter that alternates in lockstep
// with the caller's own ping-pong index -- is enough to recover, in
// onCopyDone(), which AsyncBlitState/semaphore/submit-time a completion
// belongs to (async_memcpy_isr_cb_t only carries one user cb_args
// pointer).
struct PendingOp {
  AsyncBlitState *state = nullptr;
  SemaphoreHandle_t sem = nullptr;
  int64_t submitUs = 0;
};
PendingOp gPending[2];
int gNextSlot = 0;

bool onCopyDone(async_memcpy_handle_t, async_memcpy_event_t *, void *cbArgs) {
  auto *p = (PendingOp *)cbArgs;
  p->state->busyUs += esp_timer_get_time() - p->submitUs;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(p->sem, &woken);
  return woken == pdTRUE;
}
}  // namespace

bool asyncBlitInit(AsyncBlitState *s, M5GFX *gfx) {
  auto *panel = static_cast<lgfx::Panel_DSI *>(gfx->getPanel());
  if (!panel) {
    ESP_LOGE(TAG, "no panel");
    return false;
  }

  s->panelW = panel->config().panel_width;
  s->panelH = panel->config().panel_height;
  s->panelFb = panel->config_detail().buffer;
  if (s->panelW != 720 || s->panelH != 1280 || !s->panelFb) {
    ESP_LOGE(TAG, "unexpected panel geometry %ux%u fb=%p", (unsigned)s->panelW,
             (unsigned)s->panelH, s->panelFb);
    s->ok = false;
    return false;
  }
  // Panel_DSI::init() computes line_length = ((panel_width*bits>>3)+3)&~3;
  // for 720px * 16bpp that's already 1440, a multiple of 4, so the
  // framebuffer is a plain contiguous 720x1280 raster with no per-row
  // padding -- row offset is simply logicalY * rowBytes.
  s->rowBytes = (size_t)s->panelW * sizeof(uint16_t);

  // PSRAM (both the strip buffers and this panel framebuffer live there)
  // hangs off the AXI bus matrix on ESP32-P4, not the AHB one, so use
  // AXI-GDMA explicitly rather than esp_async_memcpy_install()'s default
  // backend selection (which would pick AHB-GDMA on this chip, since P4
  // has no CPDMA).
  async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
  cfg.backlog = 4;  // comfortably more than our 2-deep ping-pong pipeline
  async_memcpy_handle_t handle = nullptr;
  if (esp_async_memcpy_install_gdma_axi(&cfg, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "esp_async_memcpy_install_gdma_axi failed");
    s->ok = false;
    return false;
  }

  s->client = handle;
  s->ok = true;
  return true;
}

bool asyncBlitSubmitStripAsync(AsyncBlitState *s, uint16_t logicalY,
                               uint16_t numLines, uint16_t logicalW,
                               const uint16_t *stripBuf,
                               SemaphoreHandle_t doneSem) {
  if (!s->ok) return false;

  // The strip buffer was written by the CPU; the DMA engine reads it as a
  // bus master, which bypasses the CPU data cache, so the write-back must
  // happen before the transaction is submitted.
  size_t bytes = (size_t)logicalW * numLines * sizeof(uint16_t);
  esp_cache_msync(
      (void *)stripBuf, bytes,
      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

  int slot = gNextSlot;
  gNextSlot ^= 1;
  gPending[slot] = {s, doneSem, esp_timer_get_time()};

  uint8_t *dst = (uint8_t *)s->panelFb + (size_t)logicalY * s->rowBytes;
  esp_err_t err =
      esp_async_memcpy((async_memcpy_handle_t)s->client, dst, (void *)stripBuf,
                       bytes, onCopyDone, &gPending[slot]);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_async_memcpy failed: %d", (int)err);
    return false;
  }
  return true;
}

}  // namespace lcdtap::m5tab5
