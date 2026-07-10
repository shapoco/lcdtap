#pragma once
#include <cstdint>

#include <M5GFX.h>

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

struct DisplayOutState {
  M5GFX *gfx;
  uint16_t width, height;
  uint16_t stripLines;
  uint16_t *strip;  // PSRAM
  uint32_t frameCount;

  // Cumulative timing (microseconds, since boot) for bottleneck analysis.
  // Callers diff these against a periodic snapshot to report per-frame
  // averages, the same way frameCount is diffed to compute fps.
  uint64_t fillUs;    // time spent in the per-scanline fill() callback
  uint64_t stripUs;   // time spent in the per-strip stripFn() callback
  uint64_t submitUs;  // time spent in gfx->pushImage()
};

// Allocate the strip buffer and cache the output geometry.
// The display itself must already be initialized (M5.begin() + setRotation()).
bool displayOutInit(DisplayOutState *s, M5GFX *gfx,
                    const DisplayOutConfig &cfg);

// Render one full frame strip by strip and push it to the panel.
// fill() is called per scanline; stripFn (optional, may be nullptr) is
// called per strip for overlay compositing.
void displayOutRenderFrame(DisplayOutState *s, FillScanlineFn fill,
                           FillStripFn stripFn, void *userData);

}  // namespace lcdtap::m5tab5
