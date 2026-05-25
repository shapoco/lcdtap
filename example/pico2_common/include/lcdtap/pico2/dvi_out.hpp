#pragma once
#include <cstdint>

#include "dvi.h"
#include "lcdtap/lcdtap.hpp"

namespace lcdtap::pico2 {

// Callback invoked by Core 1 after LcdTap::fillScanline for each scanline.
// Use for OSD overlay or other post-processing. nullptr = disabled.
using DviFillFunc = void (*)(uint16_t scanY, uint16_t *buf, void *userData);

struct DviOutConfig {
  uint pinLed;
  uint32_t ledToggleFrames;
};

struct DviOutState {
  DviOutConfig cfg;
  dvi_inst *dvi;
  uint16_t *scanBuf0;  // pointer to first element of scanlineBufs[0]
  uint32_t
      scanBufStride;  // bytes between rows (= DVI_MAX_W * sizeof(uint16_t))
  DviFillFunc fillFn;
  void *fillUserData;
  lcdtap::LcdTap *inst;  // nullptr = black frame output

  volatile uint32_t dviH;  // set by Core 0 before dviOutLaunchCore1
  volatile bool newFrame;  // set by Core 1 at each frame boundary

  // Flash-write cooperative pause (see dviOutFlashAcquire /
  // dviOutFlashRelease).
  volatile bool flashPauseReq;  // Core 0 sets to request pause
  volatile bool flashPauseAck;  // Core 1 sets when paused at safe boundary
};

// Store state and enqueue all scanline buffers into q_colour_free.
// Call after dvi_init() and before dviOutLaunchCore1().
// scanBuf0 = scanlineBufs[0] (decays to uint16_t*).
// scanBufStride = sizeof(scanlineBufs[0]).
void dviOutPrepare(DviOutState *s, dvi_inst *dvi, uint16_t *scanBuf0,
                   uint32_t scanBufStride, lcdtap::LcdTap *inst,
                   DviFillFunc fillFn, void *fillUserData,
                   const DviOutConfig &cfg);

// Launch Core 1 with the fillScanline + TMDS encode loop.
// s->dviH must be set before calling this.
// Only one DviOutState instance can be active at a time.
void dviOutLaunchCore1(DviOutState *s);

// Consume the new-frame flag. Returns true once per frame boundary.
bool dviOutConsumeNewFrame(DviOutState *s);

// Pause Core 1 at a safe SRAM-resident boundary so Core 0 can call
// flash_range_erase / flash_range_program without XIP conflicts.
// Core 1 yields between scanlines (DMA_IRQ_0 remains active during the pause).
// Must be followed by dviOutFlashRelease() after the flash operation.
void dviOutFlashAcquire(DviOutState *s);

// Resume Core 1 after a flash write initiated by dviOutFlashAcquire().
void dviOutFlashRelease(DviOutState *s);

}  // namespace lcdtap::pico2
