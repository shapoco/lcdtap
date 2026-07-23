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
#include "lcdtap/pico2/composite_sink.hpp"
#include "lcdtap/pico2/composite_timing.hpp"

namespace lcdtap::pico2 {

// Callback invoked by Core 1 after LcdTap::fillScanline for each active line.
// Signature matches HstxFillFunc so hosts can share one overlay function.
using CompositeFillFunc = void (*)(uint16_t scanY, uint16_t *buf,
                                   void *userData);

// Number of line-group buffers in the DMA ring. Each slot holds linesPerSlot
// lines, so Core 1 has (NUM_SLOTS-1) * linesPerSlot lines of fill budget
// before a buffer it is still writing could be read back out. Six (not four)
// leaves extra slack to ride out a transient Core 1 stall -- e.g. a burst of
// host input contending the bus -- on top of the SRAM-resident encode. Uses
// one DMA channel each; well within the 16 the RP2350 has.
static constexpr int COMPOSITE_NUM_SLOTS = 6;

// Optional Core 1 fill-time instrumentation, for validating that a chroma
// mode meets the slot deadline (see plan_direct_yuv.md). When enabled, Core 1
// times every slot refill with the DWT cycle counter and keeps last/max in
// CompositeOutState. Off by default: the counters themselves are cheap, but
// perf numbers should never silently ship in a release build.
//   cmake: target_compile_definitions(... COMPOSITE_PERF_STATS=1)
#ifndef COMPOSITE_PERF_STATS
#define COMPOSITE_PERF_STATS 0
#endif

struct CompositeOutConfig {
  uint32_t pinLed;
  uint32_t ledToggleFrames;
  // Which DAC drives the output: the R-2R ladder or the PWM pin.
  const CompositeDacProfile *dac;
  // Active-pixel conversion path. LUT is the production default; the DIRECT
  // modes are the RGB565-resolution experiment and must not ship as default
  // until the cycle budget is confirmed on hardware.
  CompositeChromaMode chromaMode = CompositeChromaMode::LUT;
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

  // Diagnostics: incremented by the DMA IRQ when it hands a slot back to
  // Core 1 whose previous fill request is still pending -- i.e. the hardware
  // just transmitted stale samples because Core 1 missed the deadline. Zero
  // in healthy operation; any nonzero value means the current chroma mode /
  // load combination does not fit the budget.
  volatile uint32_t underrunCount;

  // COMPOSITE_PERF_STATS only (otherwise left at zero): per-slot refill time
  // in sysclk cycles, measured on Core 1 around fillScanline + the fillFn
  // overlay + compositeEmitLine for all lines of a slot. Compare against the
  // slot period, samplesPerLine * linesPerSlot * clkPerSample cycles.
  volatile uint32_t perfSlotLast;
  volatile uint32_t perfSlotMax;
  volatile uint32_t perfSlotCount;

  // Fill requests from the DMA IRQ to the Core 1 loop. Same-core handshake:
  // the IRQ writes fillLine[] then sets fillPending[]; single-byte writes are
  // atomic on Cortex-M33.
  volatile uint8_t fillPending[COMPOSITE_NUM_SLOTS];
  uint16_t fillLine[COMPOSITE_NUM_SLOTS];  // first field line held by a slot

  // --- internal ---
  const CompositeSink *sink;
  CompositeEncoder enc;
  // COMPOSITE_NUM_SLOTS * enc.bytesPerSlot, malloc'd. Byte-granular: the two
  // sinks use different transfer widths, so all slot arithmetic is in bytes.
  uint8_t *slotBufs;
  uint32_t *lut;         // chroma LUT, malloc'd
  uint16_t *rgbScratch;  // one line of RGB565, reused by every active line
  uint32_t dmaChannels[COMPOSITE_NUM_SLOTS];
  // Slot base addresses, precomputed so the DMA IRQ can rewind read_addr with
  // a plain load. The handler lives in scratch SRAM and must not risk a call
  // into flash-resident code, so it must not do the pointer arithmetic itself.
  uint32_t slotAddr[COMPOSITE_NUM_SLOTS];
  uint32_t chMask;  // bitmask of the claimed DMA channels
  union {
    struct {
      uint32_t sm;
      uint32_t offset;
    } pio;
    struct {
      uint32_t slice;
      uint32_t channel;
    } pwm;
  } sinkState;
  volatile uint32_t groupLine;  // field line at the head of the next group
  uint32_t slotNum;             // index of the slot that last completed
  uint32_t frame;
  bool led;
};

// Byte offset of slot `i`. All three call sites must go through this: the
// slot stride is bytesPerSlot, not a count of uint32_t.
inline uint8_t *compositeSlotPtr(const CompositeOutState *s, uint32_t i) {
  return s->slotBufs + (size_t)i * s->enc.bytesPerSlot;
}

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

// The state whose Core 1 is currently running, or nullptr before
// compositeOutLaunchCore1(). Only one instance can be active; this exists so
// diagnostics (underrunCount, perfSlot*) can be read without plumbing the
// state pointer through every consumer.
CompositeOutState *compositeOutActiveState();

// Stop Core 1, the PIO SM and all DMA so Core 0 can safely erase/program
// flash. Video output stops. Must be paired with compositeOutFlashRelease().
void compositeOutFlashAcquire(CompositeOutState *s);

// Restart PIO, DMA and Core 1 after a flash write. Restores QMI timing.
void compositeOutFlashRelease(CompositeOutState *s);

}  // namespace lcdtap::pico2
