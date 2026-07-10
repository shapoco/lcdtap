#pragma once
#include <cstdint>

#include <M5GFX.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "lcdtap/m5tab5/ppa_blit.hpp"

namespace lcdtap::m5tab5 {

// Called once per scanline to render dviWidth RGB565 pixels into dst,
// then per-strip overlays (OSD, keypad) are applied by the caller hooks.
using FillScanlineFn = void (*)(uint16_t scanY, uint16_t *dst, void *userData);

// Called once per strip after all scanlines are filled, before the strip is
// sent to the panel.
// yTop = first scanline of the strip, numLines = strip height.
using FillStripFn = void (*)(uint16_t yTop, uint16_t numLines, uint16_t *strip,
                             void *userData);

struct DisplayOutConfig {
  uint16_t stripLines;
};

// Two strip buffers (ping-pong): while the CPU fills one, the PPA hardware
// (see ppa_blit.hpp) may still be transferring the other to the panel.
static constexpr int DISPLAY_OUT_NUM_STRIP_BUFFERS = 2;

struct DisplayOutState {
  M5GFX *gfx;
  uint16_t width, height;
  uint16_t stripLines;
  // PSRAM, aligned to CONFIG_CACHE_L2_CACHE_LINE_SIZE (required by PPA /
  // esp_cache_msync).
  uint16_t *strip[DISPLAY_OUT_NUM_STRIP_BUFFERS];
  // Given once the PPA transfer reading strip[i] has completed, i.e. once
  // it is safe to refill strip[i]. Both start "given" (buffers free).
  SemaphoreHandle_t stripFree[DISPLAY_OUT_NUM_STRIP_BUFFERS];
  int curBuf;
  // True if PPA direct-to-panel-framebuffer blitting is available; false
  // falls back to the plain gfx->pushImage() path (slower, but always
  // correct) when PPA setup or panel introspection fails.
  bool usePpa;
  PpaBlitState ppa;
  uint32_t frameCount;
};

// Allocate the strip buffers and cache the output geometry.
// The display itself must already be initialized (M5.begin() + setRotation()).
bool displayOutInit(DisplayOutState *s, M5GFX *gfx,
                    const DisplayOutConfig &cfg);

// Render one full frame strip by strip and push it to the panel.
// fill() is called per scanline; stripFn (optional, may be nullptr) is
// called per strip for overlay compositing.
void displayOutRenderFrame(DisplayOutState *s, FillScanlineFn fill,
                           FillStripFn stripFn, void *userData);

}  // namespace lcdtap::m5tab5
