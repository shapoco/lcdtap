#pragma once
#include <cstdint>

#include <M5GFX.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace lcdtap::m5tab5 {

// Direct-to-panel-framebuffer strip copy using ESP32-P4 GDMA
// (esp_async_memcpy), submitted asynchronously.
//
// The panel runs at its native (portrait) orientation (see main.cpp's
// setRotation(0)), so this is a plain linear copy -- no rotation, no 2D
// block/picture semantics, unlike the PPA (SRM) approach tried earlier
// (see git history, commit 041c0bc) for the same purpose. The reason to
// offload this copy at all is that gfx->pushImage() is synchronous
// (CPU-blocking): measured on hardware, it takes ~49ms to copy a full
// 720x1280 frame regardless of content, which dominates the frame budget
// and leaves no room for the CPU's per-scanline fill work to overlap it.
// Submitting the copy asynchronously, with a double-buffered strip so the
// CPU can fill the next strip while the DMA engine transfers the previous
// one, lets that fill work hide behind the transfer instead of adding to
// it.
struct AsyncBlitState {
  void *panelFb = nullptr;
  uint16_t panelW = 0;
  uint16_t panelH = 0;
  size_t rowBytes = 0;     // physical panel framebuffer stride, in bytes
  void *client = nullptr;  // async_memcpy_handle_t, kept opaque to avoid
                           // pulling <esp_async_memcpy.h> into this header
  bool ok = false;

  // Cumulative DMA execution time (submit to completion callback), in
  // microseconds since boot. Callers diff this against a periodic
  // snapshot the same way frameCount is diffed to compute fps.
  uint64_t busyUs = 0;
};

// Installs an async-memcpy driver (AXI-GDMA -- PSRAM, which both the
// strip buffer and the panel framebuffer live in, is reachable from the
// AXI bus matrix, not the AHB one that esp_async_memcpy_install()'s
// default backend selection would pick on this chip) and locates the
// physical DSI panel framebuffer via M5GFX's public Panel_Device/
// Panel_DSI accessors (gfx->getPanel(), panel->config(),
// panel->config_detail()). `gfx` must already be initialized (M5.begin()
// + setRotation()). Returns false if the panel isn't a recognized
// 720x1280 DSI panel, or if driver installation fails; callers should
// fall back to gfx->pushImage() in that case (this example does).
bool asyncBlitInit(AsyncBlitState *s, M5GFX *gfx);

// Submits a `logicalW` x `numLines` RGB565 strip (row-major) for copying
// and returns immediately; it is copied verbatim into the physical panel
// framebuffer at row `logicalY`. `doneSem` is given (from the DMA
// driver's completion-callback context, i.e. effectively ISR context)
// once the transfer finishes reading `stripBuf` -- that is the point at
// which it becomes safe to overwrite `stripBuf` with the next strip's
// pixels.
bool asyncBlitSubmitStripAsync(AsyncBlitState *s, uint16_t logicalY,
                               uint16_t numLines, uint16_t logicalW,
                               const uint16_t *stripBuf,
                               SemaphoreHandle_t doneSem);

}  // namespace lcdtap::m5tab5
