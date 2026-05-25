#include "lcdtap/pico2/dvi_out.hpp"

#include <pico/multicore.h>

extern "C" {
#include <tmds_encode.h>
}

namespace lcdtap::pico2 {

static DviOutState *sDvi = nullptr;

static void __not_in_flash_func(core1Main)() {
  DviOutState *s = sDvi;

  dvi_register_irqs_this_core(s->dvi, DMA_IRQ_0);

  // Pre-encode one black scanline so dvi_start() has a TMDS buffer ready.
  // scanBuf0 is zero-initialised (static storage) → black frame.
  {
    const uint pixwidth = s->dvi->timing->h_active_pixels;
    const uint wpc = pixwidth / DVI_SYMBOLS_PER_WORD;
    uint32_t *tmdsbuf;
    queue_remove_blocking_u32(&s->dvi->q_tmds_free, &tmdsbuf);
    tmds_encode_data_channel_16bpp((uint32_t *)s->scanBuf0, tmdsbuf + 0 * wpc,
                                   pixwidth / 2, DVI_16BPP_BLUE_MSB,
                                   DVI_16BPP_BLUE_LSB);
    tmds_encode_data_channel_16bpp((uint32_t *)s->scanBuf0, tmdsbuf + 1 * wpc,
                                   pixwidth / 2, DVI_16BPP_GREEN_MSB,
                                   DVI_16BPP_GREEN_LSB);
    tmds_encode_data_channel_16bpp((uint32_t *)s->scanBuf0, tmdsbuf + 2 * wpc,
                                   pixwidth / 2, DVI_16BPP_RED_MSB,
                                   DVI_16BPP_RED_LSB);
    queue_add_blocking_u32(&s->dvi->q_tmds_valid, &tmdsbuf);
  }

  dvi_start(s->dvi);

  const uint pixwidth = s->dvi->timing->h_active_pixels;
  const uint wpc = pixwidth / DVI_SYMBOLS_PER_WORD;
  uint32_t scanY = 0;
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

    tmds_encode_data_channel_16bpp((uint32_t *)scanbuf, tmdsbuf + 0 * wpc,
                                   pixwidth / 2, DVI_16BPP_BLUE_MSB,
                                   DVI_16BPP_BLUE_LSB);
    tmds_encode_data_channel_16bpp((uint32_t *)scanbuf, tmdsbuf + 1 * wpc,
                                   pixwidth / 2, DVI_16BPP_GREEN_MSB,
                                   DVI_16BPP_GREEN_LSB);
    tmds_encode_data_channel_16bpp((uint32_t *)scanbuf, tmdsbuf + 2 * wpc,
                                   pixwidth / 2, DVI_16BPP_RED_MSB,
                                   DVI_16BPP_RED_LSB);

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
                   uint32_t scanBufStride, int nBufs, lcdtap::LcdTap *inst,
                   DviFillFunc fillFn, void *fillUserData,
                   const DviOutConfig &cfg) {
  s->dvi = dvi;
  s->scanBuf0 = scanBuf0;
  s->scanBufStride = scanBufStride;
  s->nBufs = nBufs;
  s->inst = inst;
  s->fillFn = fillFn;
  s->fillUserData = fillUserData;
  s->cfg = cfg;
  s->dviH = 0;
  s->newFrame = false;
  s->flashPauseReq = false;
  s->flashPauseAck = false;

  for (int i = 0; i < nBufs; ++i) {
    uint16_t *p = (uint16_t *)((uint8_t *)scanBuf0 + i * scanBufStride);
    queue_add_blocking_u32(&dvi->q_colour_free, &p);
  }
}

void dviOutLaunchCore1(DviOutState *s) {
  sDvi = s;
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
