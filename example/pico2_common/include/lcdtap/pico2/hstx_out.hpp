#pragma once

#include <cstdint>

#include "lcdtap/lcdtap.hpp"
#include "lcdtap/pico2/dvi_timing.hpp"

namespace lcdtap::pico2 {

// Callback invoked by Core 1 after LcdTap::fillScanline for each scanline.
// nullptr = disabled.
using HstxFillFunc = void (*)(uint16_t scanY, uint16_t *buf, void *userData);

// GPIO pinout for the HSTX peripheral (GPIO 12-19).
// clk_p: positive clock pin (negative = clk_p ^ 1).
// rgb_p: positive data pins [Lane0=Blue+HSync, Lane1=Green, Lane2=Red].
struct HstxPinout {
  uint8_t clk_p;
  uint8_t rgb_p[3];
};

// Pico-DVI-Sock pinout (GPIO 12-19): clk=14/15, D0=12/13, D1=18/19, D2=16/17
inline constexpr HstxPinout HSTX_PICO_SOCK_PINOUT = {14u, {12u, 18u, 16u}};

struct HstxOutConfig {
  uint32_t pinLed;
  uint32_t ledToggleFrames;
  HstxPinout pinout;
};

// Active-line DMA buffer layout: header words + pixel data.
// Header is 7 uint32_t words (3 blanking commands + 1 TMDS command).
// Pixel data is h_active_pixels uint16_t values = h_active_pixels/2 uint32_t.
static constexpr uint32_t HSTX_VACTIVE_HEADER_WORDS = 7u;
// Maximum supported h_active_pixels = 1280 (720p30).
static constexpr uint32_t HSTX_MAX_LINE_BUF_WORDS =
    HSTX_VACTIVE_HEADER_WORDS + 1280u / 2u;

// Number of DMA channels in the round-robin ring.
// Each channel gives Core 1 one extra scanline of fill budget.
// With N channels, a single fill can spike up to (N-1) scanline periods
// without causing a race condition with the DMA readout.
static constexpr int HSTX_NUM_CHANS = 8;

struct HstxOutState {
  // --- caller-visible fields ---
  HstxOutConfig cfg;
  const dvi_timing *timing;
  uint16_t dviW, dviH;
  HstxFillFunc fillFn;
  void *fillUserData;
  lcdtap::LcdTap *inst;

  volatile bool newFrame;

  // Fill requests from DMA IRQ to Core 1 main loop (same-core; IRQ sets, loop
  // clears). Single-byte per slot: single-byte writes are atomic on Cortex-M33.
  volatile uint8_t fillPending[HSTX_NUM_CHANS];  // 1 = slot needs refilling
  uint16_t fillY[HSTX_NUM_CHANS];  // y value per slot; written by IRQ before
                                   // setting pending

  // --- internal (managed by hstx_out.cpp) ---
  uint32_t
      *lineBufs;  // malloc'd DMA line buffers: HSTX_NUM_CHANS x lineBufTotalLen
  uint32_t lineBufTotalLen;              // HSTX_VACTIVE_HEADER_WORDS + dviW/2
  uint32_t dmaChannels[HSTX_NUM_CHANS];  // claimed DMA channel numbers
  volatile int vScanline;  // scanline counter (wraps at vTotalActiveLines)
  int vSyncStart;          // = v_front_porch (cached so IRQ never reads flash)
  int vSyncEnd;            // = v_front_porch + v_sync_width (cached)
  int vInactiveTotal;      // v_front_porch + v_sync_width + v_back_porch
  int vTotalActiveLines;   // vInactiveTotal + v_active_lines
  uint32_t chNum;          // index into dmaChannels[] that last completed
  uint32_t frame;
  bool led;
};

// Configure clocks for HSTX output.
// PLL_USB -> clk_sys=360 MHz, clk_usb=48 MHz, clk_peri=144 MHz.
// PLL_SYS -> clk_hstx = timing->bit_clk_khz / 2.
// Call before hstxOutInit() and before any peripheral (SPI/I2C/USB) init.
void hstxOutClockInit(const dvi_timing *timing);

// Initialize HSTX, DMA, and GPIO 12-19. Does not start DMA.
// Call after hstxOutClockInit() and after LcdTap init.
void hstxOutInit(HstxOutState *s, const dvi_timing *timing,
                 lcdtap::LcdTap *inst, HstxFillFunc fillFn, void *fillUserData,
                 const HstxOutConfig &cfg);

// Launch Core 1 with the DMA IRQ handler. Call after hstxOutInit().
// Only one HstxOutState instance can be active at a time.
void hstxOutLaunchCore1(HstxOutState *s);

// Consume the new-frame flag. Returns true once per frame boundary.
bool hstxOutConsumeNewFrame(HstxOutState *s);

// Stop Core 1 and abort all DMA channels so Core 0 can safely call
// flash_range_erase / flash_range_program.  Video output stops.
// Must be followed by hstxOutFlashRelease() after the flash operation.
void hstxOutFlashAcquire(HstxOutState *s);

// Restart HSTX, DMA, and Core 1 after a flash write initiated by
// hstxOutFlashAcquire(). Restores QMI timing and resumes video output.
void hstxOutFlashRelease(HstxOutState *s);

}  // namespace lcdtap::pico2
