#pragma once

// Composite video (NTSC/PAL) output backend.
//
// Mirrors the API surface of hstx_out.hpp so that a host can select either
// backend at boot. Core 1 generates DAC samples into a ring of line-group
// buffers; DMA streams them to a PIO state machine driving a resistor ladder.
//
// Unlike the HSTX backend this one owns clk_sys: the sample rate must be an
// exact integer division of the system clock, so NTSC and PAL each require
// their own clk_sys (315.000 / 301.500 MHz). Switching between output modes
// therefore requires a reset — see compositeOutClockInit().

#include <cstdint>

#include "lcdtap/lcdtap.hpp"
#include "lcdtap/pico2/composite_encode.hpp"
#include "lcdtap/pico2/composite_timing.hpp"

namespace lcdtap::pico2 {

// Callback invoked by Core 1 after LcdTap::fillScanline for each active line.
// Signature matches HstxFillFunc so hosts can share one overlay function.
using CompositeFillFunc = void (*)(uint16_t scanY, uint16_t *buf,
                                   void *userData);

// Number of line-group buffers in the DMA ring. Each slot holds linesPerSlot
// lines, so Core 1 has (NUM_SLOTS-1) * linesPerSlot lines of fill budget
// before a buffer it is still writing could be read back out.
static constexpr int COMPOSITE_NUM_SLOTS = 4;

struct CompositeOutConfig {
  uint32_t pinLed;
  uint32_t ledToggleFrames;
  // DAC profile: selects width (7-bit SPI / 3-bit I2C) and base GPIO.
  const CompositeDacProfile *dac;
};

struct CompositeOutState {
  // --- caller-visible ---
  CompositeOutConfig cfg;
  const CompositeTiming *timing;
  uint16_t outW, outH;
  CompositeFillFunc fillFn;
  void *fillUserData;
  lcdtap::LcdTap *inst;

  volatile bool newFrame;

  // Fill requests from the DMA IRQ to the Core 1 loop. Same-core handshake:
  // the IRQ writes fillLine[] then sets fillPending[]; single-byte writes are
  // atomic on Cortex-M33.
  volatile uint8_t fillPending[COMPOSITE_NUM_SLOTS];
  uint16_t fillLine[COMPOSITE_NUM_SLOTS];  // first field line held by a slot

  // --- internal ---
  CompositeEncoder enc;
  uint32_t *slotBufs;    // COMPOSITE_NUM_SLOTS * wordsPerSlot, malloc'd
  uint32_t *lut;         // chroma LUT, malloc'd
  uint16_t *rgbScratch;  // one line of RGB565, reused by every active line
  uint32_t dmaChannels[COMPOSITE_NUM_SLOTS];
  uint32_t pioSm;
  uint32_t pioOffset;
  volatile uint32_t groupLine;  // field line at the head of the next group
  uint32_t slotNum;             // index of the slot that last completed
  uint32_t frame;
  bool led;
};

// Configure clocks for composite output.
// Leaves pll_usb (clk_usb 48 MHz, clk_peri, clk_adc) exactly as the HSTX path
// sets it, so USB CDC keeps working; re-points clk_sys at pll_sys, programmed
// to timing->clkSysKhz. clk_hstx is left stopped.
// Call before compositeOutInit() and before any peripheral init.
void compositeOutClockInit(const CompositeTiming *timing);

// Initialize the encoder, PIO, DMA and DAC GPIOs. Does not start DMA.
// Returns false if the timing/DAC combination is unsupported or allocation
// fails. Must not be called when the LCD bus is PARALLEL: the DAC pins are
// externally driven inputs in that mode.
bool compositeOutInit(CompositeOutState *s, const CompositeTiming *timing,
                      lcdtap::LcdTap *inst, CompositeFillFunc fillFn,
                      void *fillUserData, const CompositeOutConfig &cfg);

// Launch Core 1 with the DMA IRQ handler. Call after compositeOutInit().
// Only one CompositeOutState instance can be active at a time.
void compositeOutLaunchCore1(CompositeOutState *s);

// Consume the new-frame flag. Returns true once per field.
bool compositeOutConsumeNewFrame(CompositeOutState *s);

// Stop Core 1, the PIO SM and all DMA so Core 0 can safely erase/program
// flash. Video output stops. Must be paired with compositeOutFlashRelease().
void compositeOutFlashAcquire(CompositeOutState *s);

// Restart PIO, DMA and Core 1 after a flash write. Restores QMI timing.
void compositeOutFlashRelease(CompositeOutState *s);

}  // namespace lcdtap::pico2
