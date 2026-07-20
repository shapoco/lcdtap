// PWM composite sink: one GPIO driven as a duty-modulated DAC through an RC
// filter. See composite_sink.hpp for the interface contract.
//
// The PWM period is set to the sample period, so the counter advances exactly
// once per system clock and the number of duty steps equals
// timing->clkPerSample (22 on NTSC, 17 on PAL). DMA writes one 16-bit compare
// value per sample, paced by the slice's wrap DREQ.
//
// Unlike the PIO sink there is no FIFO: a transfer that arrives more than one
// wrap period late shifts every following sample. The DMA channels are given
// high bus priority to make that as unlikely as possible.

#include <cstdint>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

#include "lcdtap/pico2/composite_out.hpp"
#include "lcdtap/pico2/composite_sink.hpp"

namespace lcdtap::pico2 {

namespace {

bool pwmAcquire(CompositeOutState *s) {
  const CompositeDacProfile *dac = s->cfg.dac;
  const uint32_t pin = dac->basePin;

  s->sinkState.pwm.slice = pwm_gpio_to_slice_num(pin);
  s->sinkState.pwm.channel = pwm_gpio_to_channel(pin);
  // A 16-bit DMA write to &pwm_hw->slice[n].cc lands in the low halfword,
  // which is channel A. An odd GPIO would be channel B and need a different
  // write address, so reject it rather than silently driving the wrong pin.
  if (s->sinkState.pwm.channel != PWM_CHAN_A) return false;

  gpio_set_function(pin, GPIO_FUNC_PWM);
  gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
  return true;
}

void pwmConfigure(CompositeOutState *s) {
  pwm_config c = pwm_get_default_config();
  // One counter tick per system clock, wrapping once per sample.
  pwm_config_set_clkdiv_int(&c, 1);
  pwm_config_set_wrap(&c, (uint16_t)(s->timing->clkPerSample - 1u));
  pwm_init(s->sinkState.pwm.slice, &c, /*start=*/false);
  // Preload blanking so the gap between configure() and the first DMA
  // transfer is a valid level rather than whatever was left in the register.
  pwm_set_chan_level(s->sinkState.pwm.slice, PWM_CHAN_A,
                     (uint16_t)s->enc.codeBlank);
}

void pwmSetEnabled(CompositeOutState *s, bool en) {
  pwm_set_enabled(s->sinkState.pwm.slice, en);
}

void pwmPark(CompositeOutState *s) {
  // Leave the slice free-running and just hold the blanking level. Stopping
  // it would freeze the pin at whatever duty was mid-flight, which is an
  // arbitrary DC offset into the receiver; holding blanking gives a clean
  // black frame instead.
  pwm_set_chan_level(s->sinkState.pwm.slice, PWM_CHAN_A,
                     (uint16_t)s->enc.codeBlank);
}

uint32_t pwmDreq(const CompositeOutState *s) {
  return pwm_get_dreq(s->sinkState.pwm.slice);
}

volatile void *pwmWriteAddr(const CompositeOutState *s) {
  return &pwm_hw->slice[s->sinkState.pwm.slice].cc;
}

}  // namespace

const CompositeSink COMPOSITE_SINK_PWM = {
    .name = "PWM",
    .kind = CompositeDacKind::PWM,
    .acquire = pwmAcquire,
    .configure = pwmConfigure,
    // The slice keeps running across a flash write; park() holds blanking.
    .setEnabled = pwmSetEnabled,
    .park = pwmPark,
    .dreq = pwmDreq,
    .writeAddr = pwmWriteAddr,
    .dmaTransferSize = DMA_SIZE_16,
    // No FIFO to hide a late transfer, so win bus arbitration.
    .dmaHighPriority = true,
};

const CompositeSink *compositeSinkFor(const CompositeDacProfile *dac) {
  return dac->kind == CompositeDacKind::PWM ? &COMPOSITE_SINK_PWM
                                            : &COMPOSITE_SINK_R2R;
}

}  // namespace lcdtap::pico2
