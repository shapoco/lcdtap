#pragma once
#include <cstdint>

#include <M5GFX.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace lcdtap::m5tab5 {

// Direct-to-panel-framebuffer blit using the ESP32-P4 PPA (Pixel Processing
// Accelerator) hardware. Rotates a logical/landscape RGB565 strip and
// writes it straight into the physical MIPI-DSI panel framebuffer, bypassing
// M5GFX's pushImage()/writeImage() -- which, at setRotation(3) (needed
// because the Tab5 panel is physically portrait but this app's canvas is
// landscape), falls back to a slow generic per-pixel copy instead of a bulk
// memcpy, and is the dominant cost of the ~10 FPS baseline.
struct PpaBlitState {
  void *panelFb = nullptr;
  uint16_t panelW = 0;
  uint16_t panelH = 0;
  void *client = nullptr;  // ppa_client_handle_t, kept opaque to avoid
                           // pulling <driver/ppa.h> into this public header
  bool ok = false;
};

// Registers a PPA SRM client and locates the physical DSI panel framebuffer
// via M5GFX's public Panel_Device/Panel_DSI accessors (gfx->getPanel(),
// panel->config(), panel->config_detail()). `gfx` must already be
// initialized (M5.begin() + setRotation()) but its rotation is otherwise
// irrelevant here -- only the panel's native (physical) geometry is used.
// Returns false if the panel isn't a recognized 720x1280 DSI panel, or if
// PPA client registration fails; callers should fall back to
// gfx->pushImage() in that case (this example does).
bool ppaBlitInit(PpaBlitState *s, M5GFX *gfx);

// Rotates a `logicalW` x `numLines` RGB565 strip (row-major, in the same
// logical/landscape coordinate space as gfx->width()/height(), i.e. what
// M5.Display.setRotation(3) currently establishes) and writes it directly
// into the physical panel framebuffer at the corresponding column band.
// `logicalY` is the strip's first scanline in that logical coordinate
// space. Blocks until the PPA transaction completes.
bool ppaBlitStripBlocking(PpaBlitState *s, uint16_t logicalY, uint16_t numLines,
                          uint16_t logicalW, const uint16_t *stripBuf);

// Non-blocking variant: submits the strip to PPA and returns immediately.
// `doneSem` is given (from the PPA driver's completion-callback context,
// i.e. effectively ISR context) once the transfer finishes reading
// `stripBuf` -- that is the point at which it becomes safe to overwrite
// `stripBuf` with the next strip's pixels.
bool ppaBlitSubmitStripAsync(PpaBlitState *s, uint16_t logicalY,
                             uint16_t numLines, uint16_t logicalW,
                             const uint16_t *stripBuf,
                             SemaphoreHandle_t doneSem);

}  // namespace lcdtap::m5tab5
