#include "lcdtap/pico2/dvi_out.hpp"

#include <hardware/structs/bus_ctrl.h>
#include <pico/multicore.h>

extern "C" {
#include <tmds_encode.h>
}

namespace lcdtap::pico2 {

static DviOutState *sDvi = nullptr;

static void __not_in_flash_func(core1Main)() {
  DviOutState *s = sDvi;

  dvi_register_irqs_this_core(s->dvi, DMA_IRQ_0);

  const uint pixwidth = s->dvi->timing->h_active_pixels;
  const uint wpc = pixwidth / DVI_SYMBOLS_PER_WORD;

  // Pre-encode scanline 0 so dvi_start() has a TMDS buffer ready with correct
  // content. Encoding black here would cause a permanent 1-line vertical offset
  // because the extra slot shifts all subsequent scanlines by one.
  {
    if (s->inst) s->inst->fillScanline(0, s->scanBuf0);
    if (s->fillFn) s->fillFn(0, s->scanBuf0, s->fillUserData);
    uint32_t *tmdsbuf;
    queue_remove_blocking_u32(&s->dvi->q_tmds_free, &tmdsbuf);

#if DVI_SYMBOLS_PER_WORD == 2
    tmds_encode_data_channel_16bpp((uint32_t *)s->scanBuf0, tmdsbuf + 0 * wpc,
                                   wpc, DVI_16BPP_BLUE_MSB, DVI_16BPP_BLUE_LSB);
    tmds_encode_data_channel_16bpp((uint32_t *)s->scanBuf0, tmdsbuf + 1 * wpc,
                                   wpc, DVI_16BPP_GREEN_MSB,
                                   DVI_16BPP_GREEN_LSB);
    tmds_encode_data_channel_16bpp((uint32_t *)s->scanBuf0, tmdsbuf + 2 * wpc,
                                   wpc, DVI_16BPP_RED_MSB, DVI_16BPP_RED_LSB);
#else
    tmds_encode_data_channel_fullres_16bpp(
        (uint32_t *)s->scanBuf0, tmdsbuf + 0 * wpc, wpc, DVI_16BPP_BLUE_MSB,
        DVI_16BPP_BLUE_LSB);
    tmds_encode_data_channel_fullres_16bpp(
        (uint32_t *)s->scanBuf0, tmdsbuf + 1 * wpc, wpc, DVI_16BPP_GREEN_MSB,
        DVI_16BPP_GREEN_LSB);
    tmds_encode_data_channel_fullres_16bpp(
        (uint32_t *)s->scanBuf0, tmdsbuf + 2 * wpc, wpc, DVI_16BPP_RED_MSB,
        DVI_16BPP_RED_LSB);
#endif
    queue_add_blocking_u32(&s->dvi->q_tmds_valid, &tmdsbuf);
  }

  dvi_start(s->dvi);

  uint32_t scanY = 1;  // scanY=0 was already pre-encoded above
  uint32_t frame = 0;
  bool led = false;

  while (true) {
    uint16_t *scanbuf;
    queue_remove_blocking_u32(&s->dvi->q_colour_free, &scanbuf);

    // Cooperative pause for flash writes. Core 1 yields here (before any
    // flash-resident call) so Core 0 can safely run flash_range_erase /
    // flash_range_program. DMA_IRQ_0 remains active during the WFE spin,
    // keeping the PicoDVI DMA pipeline from losing queue coherence.
    if (s->flashPauseReq) {
      queue_add_blocking_u32(&s->dvi->q_colour_free, &scanbuf);
      s->flashPauseAck = true;
      __dmb();
      __sev();
      while (s->flashPauseReq) __wfe();
      __dmb();
      s->flashPauseAck = false;
      continue;
    }

    if (s->inst) s->inst->fillScanline(static_cast<uint16_t>(scanY), scanbuf);
    if (s->fillFn)
      s->fillFn(static_cast<uint16_t>(scanY), scanbuf, s->fillUserData);

    uint32_t *tmdsbuf;
    queue_remove_blocking_u32(&s->dvi->q_tmds_free, &tmdsbuf);

#if DVI_SYMBOLS_PER_WORD == 2
    tmds_encode_data_channel_16bpp((uint32_t *)scanbuf, tmdsbuf + 0 * wpc, wpc,
                                   DVI_16BPP_BLUE_MSB, DVI_16BPP_BLUE_LSB);
    tmds_encode_data_channel_16bpp((uint32_t *)scanbuf, tmdsbuf + 1 * wpc, wpc,
                                   DVI_16BPP_GREEN_MSB, DVI_16BPP_GREEN_LSB);
    tmds_encode_data_channel_16bpp((uint32_t *)scanbuf, tmdsbuf + 2 * wpc, wpc,
                                   DVI_16BPP_RED_MSB, DVI_16BPP_RED_LSB);
#else
    tmds_encode_data_channel_fullres_16bpp(
        (uint32_t *)scanbuf, tmdsbuf + 0 * wpc, wpc, DVI_16BPP_BLUE_MSB,
        DVI_16BPP_BLUE_LSB);
    tmds_encode_data_channel_fullres_16bpp(
        (uint32_t *)scanbuf, tmdsbuf + 1 * wpc, wpc, DVI_16BPP_GREEN_MSB,
        DVI_16BPP_GREEN_LSB);
    tmds_encode_data_channel_fullres_16bpp(
        (uint32_t *)scanbuf, tmdsbuf + 2 * wpc, wpc, DVI_16BPP_RED_MSB,
        DVI_16BPP_RED_LSB);
#endif

    queue_add_blocking_u32(&s->dvi->q_tmds_valid, &tmdsbuf);
    queue_add_blocking_u32(&s->dvi->q_colour_free, &scanbuf);

    if (++scanY >= s->dviH) {
      scanY = 0;
      __dmb();
      s->newFrame = true;
      if (++frame % s->cfg.ledToggleFrames == 0u) {
        led = !led;
        gpio_put(s->cfg.pinLed, led ? 1 : 0);
      }
    }
  }
}

void dviOutPrepare(DviOutState *s, dvi_inst *dvi, uint16_t *scanBuf0,
                   uint32_t scanBufStride, lcdtap::LcdTap *inst,
                   DviFillFunc fillFn, void *fillUserData,
                   const DviOutConfig &cfg) {
  s->dvi = dvi;
  s->scanBuf0 = scanBuf0;
  s->scanBufStride = scanBufStride;
  s->inst = inst;
  s->fillFn = fillFn;
  s->fillUserData = fillUserData;
  s->cfg = cfg;
  s->dviH = 0;
  s->newFrame = false;
  s->flashPauseReq = false;
  s->flashPauseAck = false;

  for (int i = 0; i < DVI_N_TMDS_BUFFERS; ++i) {
    uint16_t *p = (uint16_t *)((uint8_t *)scanBuf0 + i * scanBufStride);
    queue_add_blocking_u32(&dvi->q_colour_free, &p);
  }
}

void dviOutLaunchCore1(DviOutState *s) {
  sDvi = s;
  hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
  multicore_launch_core1(core1Main);
}

bool dviOutConsumeNewFrame(DviOutState *s) {
  if (s->newFrame) {
    s->newFrame = false;
    return true;
  }
  return false;
}

void dviOutFlashAcquire(DviOutState *s) {
  s->flashPauseReq = true;
  __dmb();
  __sev();  // wake Core 1 if it is in WFE inside a queue wait
  while (!s->flashPauseAck) __wfe();
}

void dviOutFlashRelease(DviOutState *s) {
  s->flashPauseReq = false;
  __dmb();
  __sev();  // wake Core 1 from its pause WFE
}

}  // namespace lcdtap::pico2
