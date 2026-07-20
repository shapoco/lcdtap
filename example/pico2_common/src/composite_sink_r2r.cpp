// R-2R composite sink: a PIO state machine shifts packed samples onto a
// 7-bit resistor ladder. See composite_sink.hpp for the interface contract.

#include <cstdint>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "lcdtap/pico2/composite_out.hpp"
#include "lcdtap/pico2/composite_sink.hpp"

#include "composite_dac.pio.h"

namespace lcdtap::pico2 {

// pio0 is entirely free: the SPI/parallel slave owns pio1 SM0.
#define COMPOSITE_PIO pio0

namespace {

bool r2rAcquire(CompositeOutState *s) {
  const CompositeDacProfile *dac = s->cfg.dac;
  if (dac->bits != 7u) return false;

  const int sm = pio_claim_unused_sm(COMPOSITE_PIO, false);
  if (sm < 0) return false;
  s->sinkState.pio.sm = (uint32_t)sm;
  s->sinkState.pio.offset =
      pio_add_program(COMPOSITE_PIO, &composite_dac7_program);

  for (uint32_t i = 0; i < dac->bits; ++i) {
    const uint32_t pin = dac->basePin + i;
    pio_gpio_init(COMPOSITE_PIO, pin);
    // Lower pad impedance keeps the GPIO's own resistance a small fraction of
    // each 2k ladder leg, which is what the linearity depends on.
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
  }
  pio_sm_set_consecutive_pindirs(COMPOSITE_PIO, s->sinkState.pio.sm,
                                 dac->basePin, dac->bits, /*is_out=*/true);
  return true;
}

void r2rConfigure(CompositeOutState *s) {
  const CompositeDacProfile *dac = s->cfg.dac;
  const uint32_t sm = s->sinkState.pio.sm;

  pio_sm_config c =
      composite_dac7_program_get_default_config(s->sinkState.pio.offset);
  sm_config_set_out_pins(&c, dac->basePin, dac->bits);
  // Shift right so sample 0 sits in the least significant bits; autopull at
  // the exact number of bits the packer fills per word.
  sm_config_set_out_shift(&c, /*shift_right=*/true, /*autopull=*/true,
                          s->enc.flushBits);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  // Integer divider: one instruction (one sample) per clkPerSample cycles.
  sm_config_set_clkdiv_int_frac(&c, s->timing->clkPerSample, 0);

  pio_sm_init(COMPOSITE_PIO, sm, s->sinkState.pio.offset, &c);
}

void r2rSetEnabled(CompositeOutState *s, bool en) {
  pio_sm_set_enabled(COMPOSITE_PIO, s->sinkState.pio.sm, en);
}

void r2rPark(CompositeOutState *s) {
  const uint32_t sm = s->sinkState.pio.sm;
  pio_sm_set_enabled(COMPOSITE_PIO, sm, false);
  pio_sm_clear_fifos(COMPOSITE_PIO, sm);
  pio_sm_restart(COMPOSITE_PIO, sm);
}

uint32_t r2rDreq(const CompositeOutState *s) {
  return pio_get_dreq(COMPOSITE_PIO, s->sinkState.pio.sm, true);
}

volatile void *r2rWriteAddr(const CompositeOutState *s) {
  return &COMPOSITE_PIO->txf[s->sinkState.pio.sm];
}

}  // namespace

const CompositeSink COMPOSITE_SINK_R2R = {
    .name = "R-2R (PIO)",
    .kind = CompositeDacKind::R2R_7BIT,
    .acquire = r2rAcquire,
    .configure = r2rConfigure,
    .setEnabled = r2rSetEnabled,
    .park = r2rPark,
    .dreq = r2rDreq,
    .writeAddr = r2rWriteAddr,
    .dmaTransferSize = DMA_SIZE_32,
    // The PIO TX FIFO absorbs DMA jitter, so no priority boost is needed.
    .dmaHighPriority = false,
};

}  // namespace lcdtap::pico2
