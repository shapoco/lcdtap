#include "lcdtap/pico2/composite_out.hpp"

#include <cstdlib>
#include <cstring>

#include <pico/multicore.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pll.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"

#include "lcdtap/pico2/composite_sink.hpp"

namespace lcdtap::pico2 {

static constexpr int NUM_SLOTS = COMPOSITE_NUM_SLOTS;

static CompositeOutState *sCvbs = nullptr;

// ---------------------------------------------------------------------------
// DMA IRQ handler — runs on Core 1 from scratch SRAM.
// Must not access flash: all code and data referenced here are in SRAM.
// ---------------------------------------------------------------------------

static void __scratch_x("cvbs") compositeIrqHandler() {
  CompositeOutState *s = sCvbs;

  const uint32_t finishedIdx = s->slotNum;
  const uint32_t finishedCh = s->dmaChannels[finishedIdx];

  // Acknowledge exactly the channel that completed, not every bit currently
  // set in our mask. DMA_IRQ_0 is level-sensitive, so if a second channel
  // completed while we were getting here its flag stays raised and the
  // handler re-enters to advance the ring again -- which is what we want.
  // Clearing the whole mask would swallow that completion and leave the ring
  // permanently one slot behind the hardware.
  dma_hw->ints0 = 1u << finishedCh;

  const uint32_t nextIdx =
      (finishedIdx + 1u < (uint32_t)NUM_SLOTS) ? finishedIdx + 1u : 0u;
  s->slotNum = nextIdx;

  // Rewind the channel that just drained. TRANS_COUNT reloads itself on every
  // trigger, but READ_ADDR does not: it is left pointing one slot past the
  // buffer, so without this the ring would walk up through SRAM on each lap
  // and halt on a bus error within milliseconds. transfer_count is rewritten
  // too, matching the HSTX handler, so nothing depends on the reload
  // semantics. The finished channel is not re-triggered until the other three
  // complete, so there is ample time.
  dma_channel_hw_t *ch = &dma_hw->ch[finishedCh];
  ch->read_addr = s->slotAddr[finishedIdx];
  ch->transfer_count = s->enc.transfersPerSlot;

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

  // If this slot's previous fill request is still pending, Core 1 never got
  // to it and the pass that just completed transmitted stale samples: an
  // underrun. Count it -- this is the primary "does this chroma mode fit the
  // budget" verdict on real hardware (see plan_direct_yuv.md).
  if (s->fillPending[finishedIdx]) s->underrunCount++;

  s->fillLine[finishedIdx] = (uint16_t)line;
  __dmb();
  s->fillPending[finishedIdx] = 1;
  __sev();  // wake Core 1 main loop
}

// ---------------------------------------------------------------------------
// DWT cycle counter (Core 1 fill-time instrumentation)
// The M33 DWT is per-core, so enabling and reading it from Core 1 measures
// exactly the fill path. Raw register addresses rather than SDK structs so
// this stays self-contained and trivially SRAM-safe.
// ---------------------------------------------------------------------------

#if COMPOSITE_PERF_STATS
static inline void dwtEnable() {
  *(volatile uint32_t *)0xE000EDFCu |= 1u << 24;  // DEMCR.TRCENA
  *(volatile uint32_t *)0xE0001000u |= 1u;        // DWT_CTRL.CYCCNTENA
}
static inline uint32_t dwtCyccnt() {
  return *(volatile uint32_t *)0xE0001004u;
}
#endif

// ---------------------------------------------------------------------------
// Core 1 entry point
// ---------------------------------------------------------------------------

static void __not_in_flash_func(core1Main)() {
  CompositeOutState *s = sCvbs;

#if COMPOSITE_PERF_STATS
  dwtEnable();
#endif

  const uint32_t chMask = s->chMask;
  dma_hw->intr = chMask;
  dma_hw->ints0 = chMask;
  // Set only our bits: inte0 is shared, so a plain store would clear any
  // other channel's IRQ0 enable.
  hw_set_bits(&dma_hw->inte0, chMask);
  irq_set_exclusive_handler(DMA_IRQ_0, compositeIrqHandler);
  irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
  irq_set_enabled(DMA_IRQ_0, true);

  dma_channel_start(s->dmaChannels[0]);

  while (true) {
    bool anyWork = false;
    for (int i = 0; i < NUM_SLOTS; ++i) {
      if (!s->fillPending[i]) continue;
      anyWork = true;

#if COMPOSITE_PERF_STATS
      const uint32_t perfT0 = dwtCyccnt();
#endif
      CompositeSampleWriter w;
      uint32_t line = s->fillLine[i];
      compositeWriterInit(&w, &s->enc, compositeSlotPtr(s, i), line);
      for (uint32_t k = 0; k < s->enc.linesPerSlot; ++k, ++line) {
        uint32_t y = 0;
        const CompositeLineType type =
            compositeClassifyLine(s->timing, line, &y);
        if (type == CompositeLineType::ACTIVE) {
          if (s->inst) s->inst->fillScanline((uint16_t)y, s->rgbScratch);
          if (s->fillFn) s->fillFn((uint16_t)y, s->rgbScratch, s->fillUserData);
        }
        compositeEmitLine(&w, &s->enc, type, line, s->rgbScratch);
      }
#if COMPOSITE_PERF_STATS
      const uint32_t perfDt = dwtCyccnt() - perfT0;
      s->perfSlotLast = perfDt;
      if (perfDt > s->perfSlotMax) s->perfSlotMax = perfDt;
      s->perfSlotCount++;
#endif
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
// DMA ring, shared by both sinks
//
// One chained ring of NUM_SLOTS channels serves either sink: only the DREQ,
// the transfer width and the write target differ. Each slot is permanently
// bound to its own buffer, but READ_ADDR still has to be rewound after every
// pass -- the hardware leaves it past the end of the buffer and only
// TRANS_COUNT reloads itself. compositeIrqHandler() does that.
// ---------------------------------------------------------------------------

static void configureDmaChannels(const CompositeOutState *s) {
  for (int i = 0; i < NUM_SLOTS; ++i) {
    const uint32_t ch = s->dmaChannels[i];
    const uint32_t nextCh = s->dmaChannels[(i + 1) % NUM_SLOTS];
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_chain_to(&c, nextCh);
    channel_config_set_dreq(&c, s->sink->dreq(s));
    channel_config_set_transfer_data_size(
        &c, (dma_channel_transfer_size)s->sink->dmaTransferSize);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_high_priority(&c, s->sink->dmaHighPriority);
    dma_channel_configure(ch, &c, s->sink->writeAddr(s),
                          compositeSlotPtr(s, (uint32_t)i),
                          s->enc.transfersPerSlot, false);
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
  s->underrunCount = 0u;
  s->perfSlotLast = 0u;
  s->perfSlotMax = 0u;
  s->perfSlotCount = 0u;

  if (cfg.dac == nullptr) return false;
  s->sink = compositeSinkFor(cfg.dac);

  // The DIRECT chroma modes use no LUT (lutWords == 0); don't malloc(0).
  const uint32_t lutWords =
      compositeLutWords(timing, cfg.dac, cfg.chromaMode);
  s->lut = nullptr;
  if (lutWords != 0u) {
    s->lut = (uint32_t *)malloc(lutWords * sizeof(uint32_t));
    if (s->lut == nullptr) return false;
  }
  if (!compositeEncoderInit(&s->enc, timing, cfg.dac, s->lut, cfg.chromaMode))
    return false;

  // A field must be a whole number of groups, otherwise the ring would drift.
  if (timing->vTotalLines % s->enc.linesPerSlot != 0u) return false;

  s->slotBufs = (uint8_t *)malloc((size_t)NUM_SLOTS * s->enc.bytesPerSlot);
  s->rgbScratch =
      (uint16_t *)malloc((size_t)timing->hActivePixels * sizeof(uint16_t));
  if (s->slotBufs == nullptr || s->rgbScratch == nullptr) return false;
  memset(s->rgbScratch, 0, (size_t)timing->hActivePixels * sizeof(uint16_t));

  // Pre-fill every slot with its correct blanking/active content so that the
  // very first pass through the ring is already a valid signal.
  for (int i = 0; i < NUM_SLOTS; ++i) {
    CompositeSampleWriter w;
    uint32_t line = (uint32_t)i * s->enc.linesPerSlot;
    compositeWriterInit(&w, &s->enc, compositeSlotPtr(s, (uint32_t)i), line);
    for (uint32_t k = 0; k < s->enc.linesPerSlot; ++k, ++line) {
      uint32_t y = 0;
      compositeEmitLine(&w, &s->enc, compositeClassifyLine(timing, line, &y),
                        line, s->rgbScratch);
    }
    s->fillLine[i] = (uint16_t)((uint32_t)i * s->enc.linesPerSlot);
  }
  // The IRQ advances groupLine before handing a slot back, so seed it at the
  // last group the ring already holds.
  s->groupLine = (uint32_t)(NUM_SLOTS - 1) * s->enc.linesPerSlot;
  memset((void *)s->fillPending, 0, sizeof(s->fillPending));

  // Claim the peripheral and drive the DAC pins. Never reached in PARALLEL
  // bus mode: the caller must not get here, because both DACs sit on GPIOs
  // that the external host controller drives in that mode.
  if (!s->sink->acquire(s)) return false;
  s->sink->configure(s);

  for (int i = 0; i < NUM_SLOTS; ++i) {
    s->dmaChannels[i] = (uint32_t)dma_claim_unused_channel(true);
  }
  // Cache what the DMA IRQ needs, so the handler never computes an address or
  // touches anything that might live in flash.
  s->chMask = 0u;
  for (int i = 0; i < NUM_SLOTS; ++i) {
    s->slotAddr[i] = (uint32_t)(uintptr_t)compositeSlotPtr(s, (uint32_t)i);
    s->chMask |= 1u << s->dmaChannels[i];
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
  // The DMA ring is already configured, so the first request is serviced
  // immediately. This matters for the PWM sink, which has no FIFO to cover
  // the gap until Core 1 starts the first channel.
  s->sink->setEnabled(s, true);
  multicore_launch_core1(core1Main);
}

bool compositeOutConsumeNewFrame(CompositeOutState *s) {
  if (s->newFrame) {
    s->newFrame = false;
    return true;
  }
  return false;
}

CompositeOutState *compositeOutActiveState() { return sCvbs; }

void compositeOutFlashAcquire(CompositeOutState *s) {
  multicore_reset_core1();
  // RP2350-E5: clear EN on all channels before aborting, otherwise the abort
  // re-triggers the next channel in the chain and leaves DMA running.
  for (int i = 0; i < NUM_SLOTS; ++i) {
    hw_clear_bits(&dma_hw->ch[s->dmaChannels[i]].al1_ctrl,
                  DMA_CH0_CTRL_TRIG_EN_BITS);
  }
  for (int i = 0; i < NUM_SLOTS; ++i) {
    dma_channel_abort(s->dmaChannels[i]);
  }
  // Hold blanking for the duration of the flash write rather than freezing
  // the output at whatever sample was in flight.
  s->sink->park(s);
}

void compositeOutFlashRelease(CompositeOutState *s) {
  // flash_range_erase/program reset QMI CS0 timing via flash_exit_xip /
  // flash_enable_xip_via_boot2. Restore ours before any flash-resident code
  // (fillScanline) can run again.
  setQmiTiming();

  s->sink->setEnabled(s, false);
  s->sink->park(s);
  s->sink->configure(s);
  configureDmaChannels(s);

  // Restart from the top of the field.
  s->slotNum = 0;
  s->groupLine = (uint32_t)(NUM_SLOTS - 1) * s->enc.linesPerSlot;
  memset((void *)s->fillPending, 0, sizeof(s->fillPending));
  s->newFrame = false;

  hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
  s->sink->setEnabled(s, true);
  multicore_launch_core1(core1Main);
}

}  // namespace lcdtap::pico2
