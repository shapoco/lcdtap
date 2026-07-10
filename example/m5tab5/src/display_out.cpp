// Strip-based display output for the M5Tab5 MIPI-DSI panel.
//
// LcdTap renders one scanline at a time; this module batches STRIP_LINES
// scanlines into a strip buffer, lets the caller composite overlays (OSD,
// virtual keypad), then hands the strip to the ESP32-P4 PPA hardware
// (ppa_blit.hpp), which writes it directly into the physical DSI panel
// framebuffer asynchronously -- the panel runs at its native orientation
// (see main.cpp's setRotation(0)) so no rotation is needed, but the
// straight copy alone (measured via gfx->pushImage(), ~49ms for a full
// 720x1280 frame) is still the dominant per-frame cost. Submitting it to
// PPA asynchronously with two ping-pong strip buffers lets the CPU fill
// the next strip while PPA transfers the previous one, hiding fill work
// behind the transfer instead of adding to it.
//
// Falls back to the plain gfx->pushImage() path if PPA/panel-buffer setup
// fails (ppaBlitInit() returns false), so the example still runs (slower,
// but synchronous and always correct) if the direct-panel-buffer
// assumptions don't hold.

#include "lcdtap/m5tab5/display_out.hpp"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <sdkconfig.h>

namespace lcdtap::m5tab5 {

bool displayOutInit(DisplayOutState *s, M5GFX *gfx,
                    const DisplayOutConfig &cfg) {
  s->gfx = gfx;
  s->width = gfx->width();
  s->height = gfx->height();
  s->stripLines = cfg.stripLines;
  s->frameCount = 0;
  s->curBuf = 0;
  s->waitUs = 0;
  s->fillUs = 0;
  s->stripUs = 0;
  s->submitUs = 0;
  s->drainUs = 0;

  // Strip buffers live in PSRAM, so they must be aligned to the L2 (PSRAM)
  // cache line size, not the (smaller) L1 one -- esp_cache_msync() in
  // ppa_blit.cpp rejects anything less. width*2 is a multiple of any
  // realistic cache line size, so stripBytes stays aligned regardless of
  // STRIP_LINES.
  size_t stripBytes = (size_t)s->width * s->stripLines * sizeof(uint16_t);
  for (int i = 0; i < DISPLAY_OUT_NUM_STRIP_BUFFERS; ++i) {
    s->strip[i] = (uint16_t *)heap_caps_aligned_alloc(
        CONFIG_CACHE_L2_CACHE_LINE_SIZE, stripBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->strip[i]) return false;
    s->stripFree[i] = xSemaphoreCreateBinary();
    if (!s->stripFree[i]) return false;
    xSemaphoreGive(s->stripFree[i]);  // buffers start free
  }

  s->usePpa = ppaBlitInit(&s->ppa, gfx);
  return true;
}

void displayOutRenderFrame(DisplayOutState *s, FillScanlineFn fill,
                           FillStripFn stripFn, void *userData) {
  M5GFX *gfx = s->gfx;
  if (!s->usePpa) gfx->startWrite();

  for (uint16_t yTop = 0; yTop < s->height; yTop += s->stripLines) {
    uint16_t numLines = s->stripLines;
    if (yTop + numLines > s->height) numLines = s->height - yTop;

    int idx = s->curBuf;
    s->curBuf ^= 1;
    // Wait until PPA (or, in the fallback path, the previous pushImage)
    // is done reading this slot before overwriting it.
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s->stripFree[idx], portMAX_DELAY);
    uint16_t *strip = s->strip[idx];

    int64_t t1 = esp_timer_get_time();
    for (uint16_t i = 0; i < numLines; ++i) {
      fill((uint16_t)(yTop + i), strip + (uint32_t)i * s->width, userData);
    }

    int64_t t2 = esp_timer_get_time();
    if (stripFn) stripFn(yTop, numLines, strip, userData);

    int64_t t3 = esp_timer_get_time();
    if (s->usePpa) {
      if (!ppaBlitSubmitStripAsync(&s->ppa, yTop, numLines, s->width, strip,
                                   s->stripFree[idx])) {
        // Submit failed after init succeeded (shouldn't normally happen)
        // -- release the slot so we don't deadlock; this strip is dropped.
        xSemaphoreGive(s->stripFree[idx]);
      }
    } else {
      gfx->pushImage(0, yTop, s->width, numLines,
                     (const lgfx::rgb565_t *)strip);
      xSemaphoreGive(s->stripFree[idx]);
    }
    int64_t t4 = esp_timer_get_time();

    s->waitUs += t1 - t0;
    s->fillUs += t2 - t1;
    s->stripUs += t3 - t2;
    s->submitUs += t4 - t3;
  }

  int64_t td0 = esp_timer_get_time();
  if (s->usePpa) {
    // Drain any in-flight PPA transactions so frameCount/fps reflect full
    // frame completion, and so the next frame doesn't race a transfer
    // still in flight beyond the ping-pong depth.
    for (int i = 0; i < DISPLAY_OUT_NUM_STRIP_BUFFERS; ++i) {
      xSemaphoreTake(s->stripFree[i], portMAX_DELAY);
      xSemaphoreGive(s->stripFree[i]);
    }
  } else {
    gfx->endWrite();
  }
  s->drainUs += esp_timer_get_time() - td0;
  ++s->frameCount;
}

}  // namespace lcdtap::m5tab5
