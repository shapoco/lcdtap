// Strip-based display output for the M5Tab5 MIPI-DSI panel.
//
// LcdTap renders one scanline at a time; this module batches STRIP_LINES
// scanlines into a strip buffer, lets the caller composite overlays (OSD,
// virtual keypad), then pushes the strip with gfx->pushImage(). The panel
// is run at its native (portrait) orientation (see main.cpp's
// setRotation(0)), so pushImage()'s fast contiguous-memcpy path applies
// directly -- no manual rotation or hardware (PPA) offload is needed, and
// M5GFX handles cache write-back internally.

#include "lcdtap/m5tab5/display_out.hpp"

#include <esp_heap_caps.h>
#include <esp_timer.h>

namespace lcdtap::m5tab5 {

bool displayOutInit(DisplayOutState *s, M5GFX *gfx,
                    const DisplayOutConfig &cfg) {
  s->gfx = gfx;
  s->width = gfx->width();
  s->height = gfx->height();
  s->stripLines = cfg.stripLines;
  s->frameCount = 0;
  s->fillUs = 0;
  s->stripUs = 0;
  s->submitUs = 0;

  size_t stripBytes = (size_t)s->width * s->stripLines * sizeof(uint16_t);
  s->strip = (uint16_t *)heap_caps_malloc(stripBytes,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return s->strip != nullptr;
}

void displayOutRenderFrame(DisplayOutState *s, FillScanlineFn fill,
                           FillStripFn stripFn, void *userData) {
  M5GFX *gfx = s->gfx;
  gfx->startWrite();

  for (uint16_t yTop = 0; yTop < s->height; yTop += s->stripLines) {
    uint16_t numLines = s->stripLines;
    if (yTop + numLines > s->height) numLines = s->height - yTop;

    int64_t t0 = esp_timer_get_time();
    for (uint16_t i = 0; i < numLines; ++i) {
      fill((uint16_t)(yTop + i), s->strip + (uint32_t)i * s->width, userData);
    }

    int64_t t1 = esp_timer_get_time();
    if (stripFn) stripFn(yTop, numLines, s->strip, userData);

    int64_t t2 = esp_timer_get_time();
    gfx->pushImage(0, yTop, s->width, numLines,
                   (const lgfx::rgb565_t *)s->strip);
    int64_t t3 = esp_timer_get_time();

    s->fillUs += t1 - t0;
    s->stripUs += t2 - t1;
    s->submitUs += t3 - t2;
  }

  gfx->endWrite();
  ++s->frameCount;
}

}  // namespace lcdtap::m5tab5
