#pragma once

// Peripheral back end for composite output.
//
// composite_out.cpp owns everything that does not depend on how samples reach
// the wire: clock setup, the Core 1 fill loop, the DMA ring handshake and the
// flash acquire/release bracketing. A CompositeSink supplies only the parts
// that differ between the two DACs.
//
// Dispatch is a runtime vtable rather than a compile-time choice because both
// sinks must be linkable in one image: the selection comes from flash config
// plus bus type at boot. It costs nothing at steady state -- neither the DMA
// IRQ handler nor the Core 1 loop calls a sink member, so no sink code is ever
// reached from the SRAM-resident, flash-forbidden paths.

#include <cstdint>

#include "lcdtap/pico2/composite_timing.hpp"

namespace lcdtap::pico2 {

struct CompositeOutState;

struct CompositeSink {
  const char *name;
  CompositeDacKind kind;

  // Claim the peripheral and configure its GPIOs. Called once from
  // compositeOutInit, after the encoder is set up. Returns false on failure.
  bool (*acquire)(CompositeOutState *s);

  // Program the peripheral into its running configuration, left stopped.
  // Called from compositeOutInit and again from compositeOutFlashRelease.
  void (*configure)(CompositeOutState *s);

  // Start or stop sample consumption.
  void (*setEnabled)(CompositeOutState *s, bool en);

  // Drop anything queued inside the peripheral and drive the output at the
  // blanking level, so a stopped output is a clean black raster rather than
  // an arbitrary DC level.
  void (*park)(CompositeOutState *s);

  // DMA ring parameters.
  uint32_t (*dreq)(const CompositeOutState *s);
  volatile void *(*writeAddr)(const CompositeOutState *s);
  uint8_t dmaTransferSize;  // DMA_SIZE_32 (R-2R) / DMA_SIZE_16 (PWM)
  bool dmaHighPriority;
};

// 7-bit R-2R ladder driven by a PIO state machine (pio0).
extern const CompositeSink COMPOSITE_SINK_R2R;

// Single GPIO driven by a PWM slice.
extern const CompositeSink COMPOSITE_SINK_PWM;

// Pick the sink matching a DAC profile.
const CompositeSink *compositeSinkFor(const CompositeDacProfile *dac);

}  // namespace lcdtap::pico2
