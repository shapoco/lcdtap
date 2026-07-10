// Strip-based display output for the M5Tab5 MIPI-DSI panel via M5GFX.
//
// LcdTap renders one scanline at a time; this module batches STRIP_LINES
// scanlines into an internal-SRAM buffer, lets the caller composite
// overlays (OSD, virtual keypad), and pushes the strip with pushImage.
// Casting to lgfx::rgb565_t* declares the buffer as native-order RGB565
// so M5GFX performs whatever conversion the DSI framebuffer needs.

#include "lcdtap/m5tab5/display_out.hpp"

#include <esp_heap_caps.h>

namespace lcdtap::m5tab5 {

bool displayOutInit(DisplayOutState *s, M5GFX *gfx,
                    const DisplayOutConfig &cfg) {
  s->gfx = gfx;
  s->width = gfx->width();
  s->height = gfx->height();
  s->stripLines = cfg.stripLines;
  s->frameCount = 0;

  size_t stripBytes = (size_t)s->width * s->stripLines * sizeof(uint16_t);
  s->strip = (uint16_t *)heap_caps_malloc(
      stripBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return s->strip != nullptr;
}

void displayOutRenderFrame(DisplayOutState *s, FillScanlineFn fill,
                           FillStripFn stripFn, void *userData) {
  M5GFX *gfx = s->gfx;
  gfx->startWrite();
  for (uint16_t yTop = 0; yTop < s->height; yTop += s->stripLines) {
    uint16_t numLines = s->stripLines;
    if (yTop + numLines > s->height) numLines = s->height - yTop;

    for (uint16_t i = 0; i < numLines; ++i) {
      fill((uint16_t)(yTop + i), s->strip + (uint32_t)i * s->width, userData);
    }
    if (stripFn) stripFn(yTop, numLines, s->strip, userData);

    gfx->pushImage(0, yTop, s->width, numLines,
                   (const lgfx::rgb565_t *)s->strip);
  }
  gfx->endWrite();
  ++s->frameCount;
}

}  // namespace lcdtap::m5tab5
