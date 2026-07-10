#pragma once
#include <cstdint>

#include <M5GFX.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace lcdtap::m5tab5 {

// Direct-to-panel-framebuffer strip copy using the ESP32-P4 PPA (Pixel
// Processing Accelerator) hardware, submitted asynchronously.
//
// The panel runs at its native (portrait) orientation (see main.cpp's
// setRotation(0)), so no rotation is needed here -- gfx->pushImage() could
// already do this copy correctly. The reason to use PPA instead is that
// pushImage() is synchronous (CPU-blocking): measured on hardware, it
// takes ~49ms to copy a full 720x1280 frame regardless of content, which
// dominates the frame budget and leaves no room for the CPU's per-scanline
// fill work to overlap it. Submitting the copy to PPA asynchronously, with
// a double-buffered strip so the CPU can fill the next strip while PPA
// transfers the previous one, lets that fill work hide behind the
// transfer instead of adding to it.
struct PpaBlitState {
  void *panelFb = nullptr;
  uint16_t panelW = 0;
  uint16_t panelH = 0;
  void *client = nullptr;  // ppa_client_handle_t, kept opaque to avoid
                           // pulling <driver/ppa.h> into this public header
  bool ok = false;

  // Cumulative ESP32-P4 PPA hardware execution time (submit to completion
  // callback), in microseconds since boot. Callers diff this against a
  // periodic snapshot the same way frameCount is diffed to compute fps.
  uint64_t busyUs = 0;
};

// Registers a PPA SRM client and locates the physical DSI panel framebuffer
// via M5GFX's public Panel_Device/Panel_DSI accessors (gfx->getPanel(),
// panel->config(), panel->config_detail()). `gfx` must already be
// initialized (M5.begin() + setRotation()). Returns false if the panel
// isn't a recognized 720x1280 DSI panel, or if PPA client registration
// fails; callers should fall back to gfx->pushImage() in that case (this
// example does).
bool ppaBlitInit(PpaBlitState *s, M5GFX *gfx);

// Submits a `logicalW` x `numLines` RGB565 strip (row-major) to PPA and
// returns immediately; it is copied verbatim (no rotation/mirror/scale)
// into the physical panel framebuffer at row `logicalY`. `doneSem` is
// given (from the PPA driver's completion-callback context, i.e.
// effectively ISR context) once the transfer finishes reading `stripBuf`
// -- that is the point at which it becomes safe to overwrite `stripBuf`
// with the next strip's pixels.
bool ppaBlitSubmitStripAsync(PpaBlitState *s, uint16_t logicalY,
                             uint16_t numLines, uint16_t logicalW,
                             const uint16_t *stripBuf,
                             SemaphoreHandle_t doneSem);

}  // namespace lcdtap::m5tab5
