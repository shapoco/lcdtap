#include "lcdtap/pico2/hstx_out.hpp"

#include <cstdlib>
#include <cstring>

#include <pico/multicore.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pll.h"
#include "hardware/resets.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"

namespace lcdtap::pico2 {

static constexpr int HSTX_FIRST_PIN = 12;
static constexpr int NUM_CHANS = HSTX_NUM_CHANS;

// Command list templates: timing pixel counts are OR'd in at hstxOutInit time.
// vblank: [CMD_RAW_REPEAT|h_front_porch, sync_word,
// CMD_RAW_REPEAT|h_sync_width, sync_word, CMD_RAW_REPEAT|(h_back+h_active),
// sync_word] vactive header: same 3 blanking pairs, then
// CMD_TMDS|h_active_pixels

static const uint32_t sVblankNoSyncSrc[] = {
    HSTX_CMD_RAW_REPEAT, SYNC_V1_H1,          HSTX_CMD_RAW_REPEAT,
    SYNC_V1_H0,          HSTX_CMD_RAW_REPEAT, SYNC_V1_H1,
};
static uint32_t sVblankNoSync[count_of(sVblankNoSyncSrc)];

static const uint32_t sVblankSyncSrc[] = {
    HSTX_CMD_RAW_REPEAT, SYNC_V0_H1,          HSTX_CMD_RAW_REPEAT,
    SYNC_V0_H0,          HSTX_CMD_RAW_REPEAT, SYNC_V0_H1,
};
static uint32_t sVblankSync[count_of(sVblankSyncSrc)];

static const uint32_t sVactiveHeaderSrc[HSTX_VACTIVE_HEADER_WORDS] = {
    HSTX_CMD_RAW_REPEAT, SYNC_V1_H1, HSTX_CMD_RAW_REPEAT, SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT, SYNC_V1_H1, HSTX_CMD_TMDS,
};
static uint32_t sVactiveHeader[HSTX_VACTIVE_HEADER_WORDS];

static HstxOutState *sHstx = nullptr;

// ---------------------------------------------------------------------------
// DMA IRQ handler — runs on Core 1 from scratch SRAM.
// Must not access flash: all code and data referenced here are in SRAM.
// ---------------------------------------------------------------------------

static void __scratch_x("hstx") hstxIrqHandler() {
  HstxOutState *s = sHstx;

  const uint32_t finishedIdx = s->chNum;
  const uint32_t finishedCh = s->dmaChannels[finishedIdx];
  dma_hw->intr = 1u << finishedCh;

  const uint32_t nextIdx =
      (finishedIdx + 1u < (uint32_t)NUM_CHANS) ? finishedIdx + 1u : 0u;
  s->chNum = nextIdx;

  dma_channel_hw_t *ch = &dma_hw->ch[finishedCh];
  const int vs = s->vScanline;

  if (vs >= s->vSyncStart && vs < s->vSyncEnd) {
    ch->read_addr = (uintptr_t)sVblankSync;
    ch->transfer_count = (uint32_t)count_of(sVblankSync);
  } else if (vs < s->vInactiveTotal) {
    ch->read_addr = (uintptr_t)sVblankNoSync;
    ch->transfer_count = (uint32_t)count_of(sVblankNoSync);
  } else {
    const int y = vs - s->vInactiveTotal;
    uint32_t *lineBuf = s->lineBufs + (uint32_t)nextIdx * s->lineBufTotalLen;
    ch->read_addr = (uintptr_t)lineBuf;
    ch->transfer_count = s->lineBufTotalLen;
    s->fillY[nextIdx] = (uint16_t)y;
    __dmb();
    s->fillPending[nextIdx] = 1;
    __sev();  // wake Core 1 main loop
  }

  if (++s->vScanline == s->vTotalActiveLines) {
    s->vScanline = 0;
    __dmb();
    s->newFrame = true;
    if (++s->frame % s->cfg.ledToggleFrames == 0u) {
      s->led = !s->led;
      gpio_put(s->cfg.pinLed, s->led ? 1 : 0);
    }
  }
}

// ---------------------------------------------------------------------------
// Core 1 entry point
// ---------------------------------------------------------------------------

static void __not_in_flash_func(core1Main)() {
  HstxOutState *s = sHstx;

  uint32_t chMask = 0;
  for (int i = 0; i < NUM_CHANS; ++i) chMask |= (1u << s->dmaChannels[i]);
  dma_hw->intr = chMask;
  dma_hw->ints2 = chMask;
  dma_hw->inte2 = chMask;
  irq_set_exclusive_handler(DMA_IRQ_2, hstxIrqHandler);
  irq_set_priority(DMA_IRQ_2, PICO_HIGHEST_IRQ_PRIORITY);
  irq_set_enabled(DMA_IRQ_2, true);

  dma_channel_start(s->dmaChannels[0]);

  while (true) {
    bool anyWork = false;
    for (int i = 0; i < NUM_CHANS; ++i) {
      if (s->fillPending[i]) {
        anyWork = true;
        const uint16_t y = s->fillY[i];
        uint32_t *lineBuf = s->lineBufs + (uint32_t)i * s->lineBufTotalLen;
        uint16_t *pixels = (uint16_t *)(lineBuf + HSTX_VACTIVE_HEADER_WORDS);
        if (s->inst) s->inst->fillScanline(y, pixels);
        if (s->fillFn) s->fillFn(y, pixels, s->fillUserData);
        __dmb();
        s->fillPending[i] = 0;
      }
    }
    if (!anyWork) __wfe();
  }
}

// ---------------------------------------------------------------------------
// QMI flash timing (must not access flash itself)
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
// ---------------------------------------------------------------------------

void __no_inline_not_in_flash_func(hstxOutClockInit)(const dvi_timing *timing) {
  const uint32_t intr = save_and_disable_interrupts();

  // Slow QMI before touching PLLs to protect flash during clock transitions.
  hw_write_masked(&qmi_hw->m[0].timing, 6, QMI_M0_TIMING_CLKDIV_BITS);

  vreg_set_voltage(VREG_VOLTAGE_1_25);

  // Force a flash read so the slow QMI timing is applied before raising the
  // clock.
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

  constexpr uint32_t USB_PLL_FBDIV = 104;
  constexpr uint32_t USB_VCO_FREQ = 12 * USB_PLL_FBDIV * MHZ;
  constexpr uint32_t USB_PLL_PDIV1 = 2;
  constexpr uint32_t USB_PLL_PDIV2 = 1;
  constexpr uint32_t USB_PLL_FREQ =
      USB_VCO_FREQ / (USB_PLL_PDIV1 * USB_PLL_PDIV2);
  constexpr uint32_t CLK_SYS_FREQ = USB_PLL_FREQ / 2u;
  constexpr uint32_t CLK_PERI_FREQ = USB_PLL_FREQ / 4u;
  constexpr uint32_t CLK_USB_FREQ = 48000u * KHZ;
  constexpr uint32_t CLK_ADC_FREQ = 48000u * KHZ;

  pll_init(pll_usb, PLL_COMMON_REFDIV, USB_VCO_FREQ, USB_PLL_PDIV1,
           USB_PLL_PDIV2);

  clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                  CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, USB_PLL_FREQ,
                  CLK_SYS_FREQ);

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

  // PLL_SYS → clk_hstx = bit_clk / 2
  const uint32_t hstxClkKhz = timing->bit_clk_khz / 2u;
  uint vcoFreq, postDiv1, postDiv2;
  if (!check_sys_clock_khz(hstxClkKhz, &vcoFreq, &postDiv1, &postDiv2)) {
    panic("hstxOutClockInit: cannot achieve clk_hstx=%u kHz", hstxClkKhz);
  }
  pll_init(pll_sys, PLL_COMMON_REFDIV, vcoFreq, postDiv1, postDiv2);
  const uint32_t hstxFreq = vcoFreq / (postDiv1 * postDiv2);

  clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                  hstxFreq, hstxFreq);
}

// ---------------------------------------------------------------------------
// HSTX + DMA helpers shared between init and post-flash restart
// ---------------------------------------------------------------------------

static void configureHstxPeripheral(const HstxOutState *s) {
  hstx_ctrl_hw->expand_tmds = 4u << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
                              8u << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
                              5u << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
                              3u << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
                              4u << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
                              29u << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

  hstx_ctrl_hw->expand_shift = 2u << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
                               16u << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
                               1u << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
                               0u << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

  hstx_ctrl_hw->csr = 0;
  hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS |
                      5u << HSTX_CTRL_CSR_CLKDIV_LSB |
                      5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
                      2u << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;

  {
    const int bit = (int)s->cfg.pinout.clk_p - HSTX_FIRST_PIN;
    hstx_ctrl_hw->bit[bit] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[bit ^ 1] =
        HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
  }

  for (uint32_t lane = 0; lane < 3u; ++lane) {
    const int bit = (int)s->cfg.pinout.rgb_p[lane] - HSTX_FIRST_PIN;
    const uint32_t sel = (lane * 10u) << HSTX_CTRL_BIT0_SEL_P_LSB |
                         (lane * 10u + 1u) << HSTX_CTRL_BIT0_SEL_N_LSB;
    hstx_ctrl_hw->bit[bit] = sel;
    hstx_ctrl_hw->bit[bit ^ 1] = sel | HSTX_CTRL_BIT0_INV_BITS;
  }
}

static void configureDmaChannels(const HstxOutState *s) {
  for (int i = 0; i < NUM_CHANS; ++i) {
    const uint32_t ch = s->dmaChannels[i];
    const uint32_t nextCh = s->dmaChannels[(i + 1) % NUM_CHANS];
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_chain_to(&c, nextCh);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(ch, &c, &hstx_fifo_hw->fifo, sVblankNoSync,
                          count_of(sVblankNoSync), false);
  }
}

// ---------------------------------------------------------------------------
// HSTX + DMA initialization
// ---------------------------------------------------------------------------

void hstxOutInit(HstxOutState *s, const dvi_timing *timing,
                 lcdtap::LcdTap *inst, HstxFillFunc fillFn, void *fillUserData,
                 const HstxOutConfig &cfg) {
  s->cfg = cfg;
  s->timing = timing;
  s->inst = inst;
  s->fillFn = fillFn;
  s->fillUserData = fillUserData;
  s->dviW = (uint16_t)timing->h_active_pixels;
  s->dviH = (uint16_t)timing->v_active_lines;
  s->newFrame = false;
  s->vScanline = NUM_CHANS - 1;
  s->chNum = 0;
  s->frame = 0u;
  s->led = false;

  s->vSyncStart = timing->v_front_porch;
  s->vSyncEnd = timing->v_front_porch + timing->v_sync_width;
  s->vInactiveTotal =
      timing->v_front_porch + timing->v_sync_width + timing->v_back_porch;
  s->vTotalActiveLines = s->vInactiveTotal + timing->v_active_lines;

  // Build command lists from templates, OR-ing in pixel counts.
  memcpy(sVblankNoSync, sVblankNoSyncSrc, sizeof(sVblankNoSync));
  sVblankNoSync[0] |= (uint32_t)timing->h_front_porch;
  sVblankNoSync[2] |= (uint32_t)timing->h_sync_width;
  sVblankNoSync[4] |=
      (uint32_t)(timing->h_back_porch + timing->h_active_pixels);

  memcpy(sVblankSync, sVblankSyncSrc, sizeof(sVblankSync));
  sVblankSync[0] |= (uint32_t)timing->h_front_porch;
  sVblankSync[2] |= (uint32_t)timing->h_sync_width;
  sVblankSync[4] |= (uint32_t)(timing->h_back_porch + timing->h_active_pixels);

  memcpy(sVactiveHeader, sVactiveHeaderSrc, sizeof(sVactiveHeader));
  sVactiveHeader[0] |= (uint32_t)timing->h_front_porch;
  sVactiveHeader[2] |= (uint32_t)timing->h_sync_width;
  sVactiveHeader[4] |= (uint32_t)timing->h_back_porch;
  sVactiveHeader[6] |= (uint32_t)timing->h_active_pixels;

  // DMA line buffers: 3 slots of (header + pixel data).
  // Pixels are RGB565 uint16_t pairs packed into uint32_t words.
  s->lineBufTotalLen = HSTX_VACTIVE_HEADER_WORDS + (uint32_t)s->dviW / 2u;
  s->lineBufs = (uint32_t *)malloc((size_t)NUM_CHANS * s->lineBufTotalLen *
                                   sizeof(uint32_t));

  for (int i = 0; i < NUM_CHANS; ++i) {
    memcpy(s->lineBufs + (size_t)i * s->lineBufTotalLen, sVactiveHeader,
           sizeof(sVactiveHeader));
    memset(s->lineBufs + (size_t)i * s->lineBufTotalLen +
               HSTX_VACTIVE_HEADER_WORDS,
           0, (size_t)s->dviW * sizeof(uint16_t));
  }

  memset((void *)s->fillPending, 0, sizeof(s->fillPending));

  // Reset HSTX peripheral.
  reset_block_num(RESET_HSTX);
  sleep_us(10);
  unreset_block_num_wait_blocking(RESET_HSTX);
  sleep_us(10);

  configureHstxPeripheral(s);

  for (int pin = 12; pin <= 19; ++pin) {
    gpio_set_function(pin, GPIO_FUNC_HSTX);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_4MA);
  }

  // Claim three DMA channels and configure as a round-robin chain.
  for (int i = 0; i < NUM_CHANS; ++i) {
    s->dmaChannels[i] = (uint32_t)dma_claim_unused_channel(true);
  }

  configureDmaChannels(s);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void hstxOutLaunchCore1(HstxOutState *s) {
  sHstx = s;
  hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
  multicore_launch_core1(core1Main);
}

bool hstxOutConsumeNewFrame(HstxOutState *s) {
  if (s->newFrame) {
    s->newFrame = false;
    return true;
  }
  return false;
}

void hstxOutFlashAcquire(HstxOutState *s) {
  multicore_reset_core1();
  // RP2350-E5: clear EN on all channels before aborting to prevent auto-chain
  // re-triggering.  Our ring (ch0→ch1→ch2→ch0) means aborting without this
  // re-starts chained channels, leaving DMA running when hstxOutFlashRelease
  // calls configureDmaChannels — a 50% race against live DMA writes.
  for (int i = 0; i < NUM_CHANS; ++i) {
    hw_clear_bits(&dma_hw->ch[s->dmaChannels[i]].al1_ctrl,
                  DMA_CH0_CTRL_TRIG_EN_BITS);
  }
  for (int i = 0; i < NUM_CHANS; ++i) {
    dma_channel_abort(s->dmaChannels[i]);
  }
}

void hstxOutFlashRelease(HstxOutState *s) {
  // flash_range_erase/program reset QMI CS0 timing via flash_exit_xip /
  // flash_enable_xip_via_boot2.  Restore our 288 MHz setting before any
  // flash-resident code (fillScanline) can be called.
  setQmiTiming();

  // Reset HSTX to clear FIFO and bring peripheral to a known state.
  reset_block_num(RESET_HSTX);
  sleep_us(10);
  unreset_block_num_wait_blocking(RESET_HSTX);
  sleep_us(10);
  configureHstxPeripheral(s);

  // Reconfigure DMA channels from scratch (channels are already claimed).
  configureDmaChannels(s);

  // Reset scanline state so Core 1 starts from the beginning of the frame.
  s->vScanline = NUM_CHANS - 1;
  s->chNum = 0;
  memset((void *)s->fillPending, 0, sizeof(s->fillPending));
  s->newFrame = false;

  // Re-launch Core 1; it sets up the DMA IRQ and starts channel 0.
  hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
  multicore_launch_core1(core1Main);
}

}  // namespace lcdtap::pico2
