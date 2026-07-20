#include "lcdtap/pico2/composite_out.hpp"

#include <cstdlib>
#include <cstring>

#include <pico/multicore.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pll.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"

#include "composite_dac.pio.h"

namespace lcdtap::pico2 {

static constexpr int NUM_SLOTS = COMPOSITE_NUM_SLOTS;

// pio0 is entirely free: the SPI/parallel slave owns pio1 SM0.
#define COMPOSITE_PIO pio0

static CompositeOutState *sCvbs = nullptr;

// ---------------------------------------------------------------------------
// DMA IRQ handler — runs on Core 1 from scratch SRAM.
// Must not access flash: all code and data referenced here are in SRAM.
// ---------------------------------------------------------------------------

static void __scratch_x("cvbs") compositeIrqHandler() {
  CompositeOutState *s = sCvbs;

  const uint32_t finishedIdx = s->slotNum;
  dma_hw->intr = 1u << s->dmaChannels[finishedIdx];

  const uint32_t nextIdx =
      (finishedIdx + 1u < (uint32_t)NUM_SLOTS) ? finishedIdx + 1u : 0u;
  s->slotNum = nextIdx;

  // Unlike the HSTX ring, read_addr and transfer_count never change: every
  // slot is the same length and permanently bound to its own buffer. The
  // handler only has to hand the slot that just drained back to Core 1.
  uint32_t line = s->groupLine + s->enc.linesPerSlot;
  if (line >= s->timing->vTotalLines) {
    line = 0u;
    s->newFrame = true;
    if (++s->frame % s->cfg.ledToggleFrames == 0u) {
      s->led = !s->led;
      gpio_put(s->cfg.pinLed, s->led ? 1 : 0);
    }
  }
  s->groupLine = line;

  s->fillLine[finishedIdx] = (uint16_t)line;
  __dmb();
  s->fillPending[finishedIdx] = 1;
  __sev();  // wake Core 1 main loop
}

// ---------------------------------------------------------------------------
// Core 1 entry point
// ---------------------------------------------------------------------------

static void __not_in_flash_func(core1Main)() {
  CompositeOutState *s = sCvbs;

  uint32_t chMask = 0;
  for (int i = 0; i < NUM_SLOTS; ++i) chMask |= (1u << s->dmaChannels[i]);
  dma_hw->intr = chMask;
  dma_hw->ints0 = chMask;
  dma_hw->inte0 = chMask;
  irq_set_exclusive_handler(DMA_IRQ_0, compositeIrqHandler);
  irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
  irq_set_enabled(DMA_IRQ_0, true);

  dma_channel_start(s->dmaChannels[0]);

  while (true) {
    bool anyWork = false;
    for (int i = 0; i < NUM_SLOTS; ++i) {
      if (!s->fillPending[i]) continue;
      anyWork = true;

      CompositeSampleWriter w;
      compositeWriterInit(&w, &s->enc,
                          s->slotBufs + (size_t)i * s->enc.wordsPerSlot);
      uint32_t line = s->fillLine[i];
      for (uint32_t k = 0; k < s->enc.linesPerSlot; ++k, ++line) {
        uint32_t y = 0;
        const CompositeLineType type =
            compositeClassifyLine(s->timing, line, &y);
        if (type == CompositeLineType::ACTIVE) {
          if (s->inst) s->inst->fillScanline((uint16_t)y, s->rgbScratch);
          if (s->fillFn) s->fillFn((uint16_t)y, s->rgbScratch, s->fillUserData);
        }
        compositeEmitLine(&w, &s->enc, type, s->rgbScratch);
      }
      __dmb();
      s->fillPending[i] = 0;
    }
    if (!anyWork) __wfe();
  }
}

// ---------------------------------------------------------------------------
// QMI flash timing (must not access flash itself)
// Duplicated from hstx_out.cpp rather than shared: this runs during clock
// transitions where a cross-translation-unit call would be a flash fetch.
// ---------------------------------------------------------------------------

static void __no_inline_not_in_flash_func(setQmiTiming)() {
  while (
      (ioqspi_hw->io[1].status & IO_QSPI_GPIO_QSPI_SS_STATUS_OUTTOPAD_BITS) !=
      IO_QSPI_GPIO_QSPI_SS_STATUS_OUTTOPAD_BITS) {
  }
  qmi_hw->m[0].timing = 0x40000203u;
  volatile uint32_t *xip = (volatile uint32_t *)0x14000000u;
  (void)*xip;
}

// ---------------------------------------------------------------------------
// Clock initialization
//
// Structurally identical to hstxOutClockInit, with one difference: clk_sys is
// taken from pll_sys (free, because HSTX is off) instead of pll_usb, so that
// it can be set to the exact frequency the sample rate needs. The pll_usb
// block is left byte-for-byte the same so clk_usb stays at 48 MHz and USB CDC
// survives — that is the escape route when no TV is connected.
// ---------------------------------------------------------------------------

void __no_inline_not_in_flash_func(compositeOutClockInit)(
    const CompositeTiming *timing) {
  const uint32_t intr = save_and_disable_interrupts();

  // Slow QMI before touching PLLs to protect flash during clock transitions.
  hw_write_masked(&qmi_hw->m[0].timing, 6, QMI_M0_TIMING_CLKDIV_BITS);

  vreg_set_voltage(VREG_VOLTAGE_1_25);

  // Force a flash read so the slow QMI timing is applied before any change.
  volatile uint32_t *xip = (volatile uint32_t *)0x14000000u;
  (void)*xip;

  // Switch clk_sys and clk_ref off their aux sources before touching PLLs.
  hw_clear_bits(&clocks_hw->clk[clk_sys].ctrl, CLOCKS_CLK_SYS_CTRL_SRC_BITS);
  while (clocks_hw->clk[clk_sys].selected != 0x1u) tight_loop_contents();
  hw_write_masked(&clocks_hw->clk[clk_ref].ctrl,
                  CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC,
                  CLOCKS_CLK_REF_CTRL_SRC_BITS);
  while (clocks_hw->clk[clk_ref].selected != 0x4u) tight_loop_contents();

  clock_stop(clk_usb);
  clock_stop(clk_adc);
  clock_stop(clk_peri);
  clock_stop(clk_hstx);

  // --- pll_usb: identical to the HSTX path (clk_usb 48 MHz for USB CDC) ---
  constexpr uint32_t USB_PLL_FBDIV = 104;
  constexpr uint32_t USB_VCO_FREQ = 12 * USB_PLL_FBDIV * MHZ;
  constexpr uint32_t USB_PLL_PDIV1 = 2;
  constexpr uint32_t USB_PLL_PDIV2 = 1;
  constexpr uint32_t USB_PLL_FREQ =
      USB_VCO_FREQ / (USB_PLL_PDIV1 * USB_PLL_PDIV2);
  constexpr uint32_t CLK_PERI_FREQ = USB_PLL_FREQ / 4u;
  constexpr uint32_t CLK_USB_FREQ = 48000u * KHZ;
  constexpr uint32_t CLK_ADC_FREQ = 48000u * KHZ;

  pll_init(pll_usb, PLL_COMMON_REFDIV, USB_VCO_FREQ, USB_PLL_PDIV1,
           USB_PLL_PDIV2);

  // --- pll_sys: the exact system clock this timing needs ---
  const uint32_t vcoFreq = 12u * MHZ / timing->pllRefdiv * timing->pllFbdiv;
  const uint32_t sysFreq =
      vcoFreq / (timing->pllPostdiv1 * timing->pllPostdiv2);
  pll_init(pll_sys, timing->pllRefdiv, vcoFreq, timing->pllPostdiv1,
           timing->pllPostdiv2);

  clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                  CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, sysFreq,
                  sysFreq);

  clock_configure(clk_peri, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF,
                  CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                  USB_PLL_FREQ, CLK_PERI_FREQ);

  clock_configure(clk_usb, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF,
                  CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, USB_PLL_FREQ,
                  CLK_USB_FREQ);

  clock_configure(clk_adc, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF,
                  CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, USB_PLL_FREQ,
                  CLK_ADC_FREQ);

  setQmiTiming();

  restore_interrupts(intr);
}

// ---------------------------------------------------------------------------
// PIO / DMA helpers shared between init and post-flash restart
// ---------------------------------------------------------------------------

static void configurePioSm(const CompositeOutState *s) {
  const CompositeDacProfile *dac = s->cfg.dac;
  const uint32_t sm = s->pioSm;

  pio_sm_config c =
      (dac->bits == 7)
          ? composite_dac7_program_get_default_config(s->pioOffset)
          : composite_dac3_program_get_default_config(s->pioOffset);
  sm_config_set_out_pins(&c, dac->basePin, dac->bits);
  // Shift right so sample 0 sits in the least significant bits; autopull at
  // the exact number of bits the packer fills per word.
  sm_config_set_out_shift(&c, /*shift_right=*/true, /*autopull=*/true,
                          s->enc.flushBits);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  // Integer divider: one instruction (one sample) per clkdiv system clocks.
  sm_config_set_clkdiv_int_frac(&c, s->timing->pioClkdivInt, 0);

  pio_sm_init(COMPOSITE_PIO, sm, s->pioOffset, &c);
}

static void configureDmaChannels(const CompositeOutState *s) {
  for (int i = 0; i < NUM_SLOTS; ++i) {
    const uint32_t ch = s->dmaChannels[i];
    const uint32_t nextCh = s->dmaChannels[(i + 1) % NUM_SLOTS];
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_chain_to(&c, nextCh);
    channel_config_set_dreq(&c, pio_get_dreq(COMPOSITE_PIO, s->pioSm, true));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(ch, &c, &COMPOSITE_PIO->txf[s->pioSm],
                          s->slotBufs + (size_t)i * s->enc.wordsPerSlot,
                          s->enc.wordsPerSlot, false);
  }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool compositeOutInit(CompositeOutState *s, const CompositeTiming *timing,
                      lcdtap::LcdTap *inst, CompositeFillFunc fillFn,
                      void *fillUserData, const CompositeOutConfig &cfg) {
  s->cfg = cfg;
  s->timing = timing;
  s->inst = inst;
  s->fillFn = fillFn;
  s->fillUserData = fillUserData;
  s->outW = timing->hActivePixels;
  s->outH = timing->vActiveLines;
  s->newFrame = false;
  s->slotNum = 0;
  s->frame = 0u;
  s->led = false;

  if (cfg.dac == nullptr) return false;
  if (cfg.dac->bits != 7u && cfg.dac->bits != 3u) return false;

  s->lut = (uint32_t *)malloc(compositeLutWords(timing) * sizeof(uint32_t));
  if (s->lut == nullptr) return false;
  if (!compositeEncoderInit(&s->enc, timing, cfg.dac, s->lut)) return false;

  // A field must be a whole number of groups, otherwise the ring would drift.
  if (timing->vTotalLines % s->enc.linesPerSlot != 0u) return false;

  s->slotBufs = (uint32_t *)malloc((size_t)NUM_SLOTS * s->enc.wordsPerSlot *
                                   sizeof(uint32_t));
  s->rgbScratch =
      (uint16_t *)malloc((size_t)timing->hActivePixels * sizeof(uint16_t));
  if (s->slotBufs == nullptr || s->rgbScratch == nullptr) return false;
  memset(s->rgbScratch, 0, (size_t)timing->hActivePixels * sizeof(uint16_t));

  // Pre-fill every slot with its correct blanking/active content so that the
  // very first pass through the ring is already a valid signal.
  for (int i = 0; i < NUM_SLOTS; ++i) {
    CompositeSampleWriter w;
    compositeWriterInit(&w, &s->enc,
                        s->slotBufs + (size_t)i * s->enc.wordsPerSlot);
    uint32_t line = (uint32_t)i * s->enc.linesPerSlot;
    for (uint32_t k = 0; k < s->enc.linesPerSlot; ++k, ++line) {
      uint32_t y = 0;
      compositeEmitLine(&w, &s->enc, compositeClassifyLine(timing, line, &y),
                        s->rgbScratch);
    }
    s->fillLine[i] = (uint16_t)((uint32_t)i * s->enc.linesPerSlot);
  }
  // The IRQ advances groupLine before handing a slot back, so seed it at the
  // last group the ring already holds.
  s->groupLine = (uint32_t)(NUM_SLOTS - 1) * s->enc.linesPerSlot;
  memset((void *)s->fillPending, 0, sizeof(s->fillPending));

  // DAC pins. Never reached in PARALLEL bus mode: the caller must not call
  // this then, because GPIO5-11 are externally driven inputs there and
  // switching them to outputs would fight the host controller.
  s->pioSm = (uint32_t)pio_claim_unused_sm(COMPOSITE_PIO, true);
  s->pioOffset = (cfg.dac->bits == 7)
                     ? pio_add_program(COMPOSITE_PIO, &composite_dac7_program)
                     : pio_add_program(COMPOSITE_PIO, &composite_dac3_program);

  for (uint32_t i = 0; i < cfg.dac->bits; ++i) {
    const uint32_t pin = cfg.dac->basePin + i;
    pio_gpio_init(COMPOSITE_PIO, pin);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
  }
  pio_sm_set_consecutive_pindirs(COMPOSITE_PIO, s->pioSm, cfg.dac->basePin,
                                 cfg.dac->bits, /*is_out=*/true);
  configurePioSm(s);

  for (int i = 0; i < NUM_SLOTS; ++i) {
    s->dmaChannels[i] = (uint32_t)dma_claim_unused_channel(true);
  }
  configureDmaChannels(s);
  return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void compositeOutLaunchCore1(CompositeOutState *s) {
  sCvbs = s;
  hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
  pio_sm_set_enabled(COMPOSITE_PIO, s->pioSm, true);
  multicore_launch_core1(core1Main);
}

bool compositeOutConsumeNewFrame(CompositeOutState *s) {
  if (s->newFrame) {
    s->newFrame = false;
    return true;
  }
  return false;
}

void compositeOutFlashAcquire(CompositeOutState *s) {
  multicore_reset_core1();
  pio_sm_set_enabled(COMPOSITE_PIO, s->pioSm, false);
  // RP2350-E5: clear EN on all channels before aborting, otherwise the abort
  // re-triggers the next channel in the chain and leaves DMA running.
  for (int i = 0; i < NUM_SLOTS; ++i) {
    hw_clear_bits(&dma_hw->ch[s->dmaChannels[i]].al1_ctrl,
                  DMA_CH0_CTRL_TRIG_EN_BITS);
  }
  for (int i = 0; i < NUM_SLOTS; ++i) {
    dma_channel_abort(s->dmaChannels[i]);
  }
  pio_sm_clear_fifos(COMPOSITE_PIO, s->pioSm);
}

void compositeOutFlashRelease(CompositeOutState *s) {
  // flash_range_erase/program reset QMI CS0 timing via flash_exit_xip /
  // flash_enable_xip_via_boot2. Restore ours before any flash-resident code
  // (fillScanline) can run again.
  setQmiTiming();

  pio_sm_set_enabled(COMPOSITE_PIO, s->pioSm, false);
  pio_sm_clear_fifos(COMPOSITE_PIO, s->pioSm);
  pio_sm_restart(COMPOSITE_PIO, s->pioSm);
  configurePioSm(s);
  configureDmaChannels(s);

  // Restart from the top of the field.
  s->slotNum = 0;
  s->groupLine = (uint32_t)(NUM_SLOTS - 1) * s->enc.linesPerSlot;
  memset((void *)s->fillPending, 0, sizeof(s->fillPending));
  s->newFrame = false;

  hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
  pio_sm_set_enabled(COMPOSITE_PIO, s->pioSm, true);
  multicore_launch_core1(core1Main);
}

}  // namespace lcdtap::pico2
