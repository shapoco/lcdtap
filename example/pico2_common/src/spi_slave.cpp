#include <initializer_list>

#include "lcdtap/pico2/spi_slave.hpp"

#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include "spi_4line_mode0.pio.h"

namespace lcdtap::pico2 {

// State observed by the ring-wrap DMA IRQ. Only one SpiSlaveState can be
// active at a time (same constraint as spiSlaveRegisterIrq).
static SpiSlaveState *sWrapIrqState = nullptr;

// DMA_IRQ_1, Core 0, lowest priority: counts ring buffer wraps so that
// spiSlaveProcess can reconstruct the total number of received words.
// (DMA_IRQ_0 is used by composite output and DMA_IRQ_2 by HSTX, both on
// Core 1.)
static void __not_in_flash_func(spiRingWrapIrqHandler)() {
  SpiSlaveState *s = sWrapIrqState;
  if (!s || s->dmaCh < 0) return;
  const uint32_t chBit = 1u << (uint)s->dmaCh;
  if (dma_hw->ints1 & chBit) {
    dma_hw->ints1 = chBit;
    s->wrapCount = s->wrapCount + 1u;
  }
}

void spiSlaveInit(SpiSlaveState *s, const SpiSlaveConfig &cfg,
                  uint32_t *ringBuf, uint32_t ringWords) {
  s->cfg = cfg;
  s->ringBuf = ringBuf;
  s->ringWords = ringWords;
  s->dmaCh = -1;
  s->readIdx = 0;
  s->pioProgram = &spi_4line_mode0_program;

  for (uint pin : {cfg.pinSclk, cfg.pinMosi, cfg.pinDc}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
  }
  gpio_init(cfg.pinCs);
  gpio_set_dir(cfg.pinCs, GPIO_IN);
  gpio_pull_up(cfg.pinCs);

  uint progOffset = pio_add_program(cfg.pio, &spi_4line_mode0_program);
  s->progOffset = progOffset;

  pio_sm_config c = spi_4line_mode0_program_get_default_config(progOffset);
  sm_config_set_in_pins(&c, cfg.pinMosi);  // IN_BASE; DC is IN_BASE+1
  sm_config_set_in_shift(&c, /*shift_direction=*/false, /*autopush=*/false,
                         /*push_threshold=*/32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  sm_config_set_jmp_pin(&c, cfg.pinCs);

  pio_sm_init(cfg.pio, cfg.sm, progOffset, &c);
  pio_sm_set_enabled(cfg.pio, cfg.sm, true);

  spiSlaveInitDma(s);
}

void spiSlaveInitDma(SpiSlaveState *s) {
  s->dmaCh = dma_claim_unused_channel(true);

  dma_channel_config cfg = dma_channel_get_default_config((uint)s->dmaCh);
  channel_config_set_read_increment(&cfg, false);
  channel_config_set_write_increment(&cfg, true);
  channel_config_set_dreq(&cfg,
                          pio_get_dreq(s->cfg.pio, s->cfg.sm, /*is_tx=*/false));
  channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
  channel_config_set_ring(&cfg, /*write=*/true, s->cfg.ringLog2);

  // Ring accounting starts from a clean slate, paired with write_addr at the
  // ring base. dropWords/backlogMaxWords intentionally survive re-init.
  s->readIdx = 0;
  s->wrapCount = 0;
  s->totalConsumedWords = 0;

  // TRIGGER_SELF mode: the counter reloads to ringWords and the channel
  // re-triggers itself each time it completes, raising the channel IRQ once
  // per ring wrap. The hardware re-trigger has no dead time; incoming words
  // are additionally buffered by the joined PIO RX FIFO (8 entries).
  sWrapIrqState = s;
  static bool sWrapIrqInstalled = false;
  if (!sWrapIrqInstalled) {
    irq_set_exclusive_handler(DMA_IRQ_1, spiRingWrapIrqHandler);
    irq_set_priority(DMA_IRQ_1, PICO_LOWEST_IRQ_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);
    sWrapIrqInstalled = true;
  }
  dma_channel_set_irq1_enabled((uint)s->dmaCh, true);

  const uint32_t transCount = (DMA_CH0_TRANS_COUNT_MODE_VALUE_TRIGGER_SELF
                               << DMA_CH0_TRANS_COUNT_MODE_LSB) |
                              s->ringWords;
  dma_channel_configure((uint)s->dmaCh, &cfg, s->ringBuf,
                        &s->cfg.pio->rxf[s->cfg.sm], transCount,
                        /*trigger=*/true);
}

void spiSlaveRegisterIrq(SpiSlaveState *s) {
  gpio_set_irq_enabled(s->cfg.pinCs, GPIO_IRQ_EDGE_RISE, true);
}

void spiSlaveDeinit(SpiSlaveState *s) {
  gpio_set_irq_enabled(s->cfg.pinCs, GPIO_IRQ_EDGE_RISE, false);
  if (s->dmaCh >= 0) {
    // RP2350-E5: clear EN before aborting a self-triggering channel, or the
    // abort can re-trigger it (same mitigation as hstxOutFlashAcquire).
    hw_clear_bits(&dma_hw->ch[s->dmaCh].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_set_irq1_enabled((uint)s->dmaCh, false);
    dma_channel_abort((uint)s->dmaCh);
    dma_channel_unclaim((uint)s->dmaCh);
    s->dmaCh = -1;
  }
  pio_sm_set_enabled(s->cfg.pio, s->cfg.sm, false);
  pio_sm_clear_fifos(s->cfg.pio, s->cfg.sm);
  if (s->pioProgram) {
    pio_remove_program(s->cfg.pio, s->pioProgram, s->progOffset);
    s->pioProgram = nullptr;
  }
  s->readIdx = 0;
}

void spiSlaveResetSm(SpiSlaveState *s) {
  pio_sm_set_enabled(s->cfg.pio, s->cfg.sm, false);
  pio_sm_clear_fifos(s->cfg.pio, s->cfg.sm);
  pio_sm_restart(s->cfg.pio, s->cfg.sm);
  pio_sm_exec(s->cfg.pio, s->cfg.sm, pio_encode_jmp(s->progOffset));
  pio_sm_set_enabled(s->cfg.pio, s->cfg.sm, true);
}

void __not_in_flash_func(spiSlaveProcess)(SpiSlaveState *s) {
  dma_channel_hw_t *dmaHw = dma_channel_hw_addr((uint)s->dmaCh);

  // --- Ring overrun detection (once per call, outside the data loop) ---
  {
    const uint32_t chBit = 1u << (uint)s->dmaCh;
    uint32_t wrap, pend, writeIdxNow;
    // Re-sample until wrapCount and the pending-IRQ flag are stable, so
    // writeIdxNow is guaranteed to belong to the (wrap + pend) epoch. A
    // second wrap cannot occur within this window (one wrap takes >= 0.5 ms
    // even at the maximum input rate).
    do {
      wrap = s->wrapCount;
      pend = (dma_hw->ints1 & chBit) ? 1u : 0u;
      writeIdxNow =
          ((dmaHw->write_addr - (uint32_t)s->ringBuf) / sizeof(uint32_t)) &
          (s->ringWords - 1u);
    } while (wrap != s->wrapCount ||
             pend != ((dma_hw->ints1 & chBit) ? 1u : 0u));

    const uint32_t totalReceived = (wrap + pend) * s->ringWords + writeIdxNow;
    uint32_t backlog = totalReceived - s->totalConsumedWords;
    if (backlog & 0x80000000u) {
      // Wrap IRQ starved long enough to miss a full ring period; the
      // received-word estimate fell behind consumption. Resync silently
      // rather than reporting a bogus drop.
      s->totalConsumedWords = totalReceived;
      backlog = 0;
    }
    if (backlog > s->ringWords) {
      // The DMA lapped the consumer: everything currently in the ring is an
      // inconsistent mix of old and new bytes. Count the overshoot (a lower
      // bound when wrap IRQs were suppressed, e.g. during a flash write) and
      // resynchronize to the write pointer.
      s->dropWords += backlog - s->ringWords;
      s->readIdx = writeIdxNow;
      s->totalConsumedWords = totalReceived;
      backlog = 0;
    }
    if (backlog > s->backlogMaxWords) s->backlogMaxWords = backlog;
  }

  for (int i = 0; i < 3; i++) {
    uint32_t writeAddr = dmaHw->write_addr;
    uint32_t writeIdx =
        (writeAddr - reinterpret_cast<uint32_t>(s->ringBuf)) / sizeof(uint32_t);
    writeIdx &= (s->ringWords - 1u);

    if (!s->inst) {
      s->totalConsumedWords += (writeIdx - s->readIdx) & (s->ringWords - 1u);
      s->readIdx = writeIdx;
      return;
    }

    const uint32_t iterStartIdx = s->readIdx;

    uint32_t readIdx = s->readIdx;
    uint32_t dataStart = readIdx;
    while (readIdx != writeIdx) {
      uint32_t lastReadIdx = readIdx;
      uint32_t word = s->ringBuf[readIdx];
      readIdx = (readIdx + 1u) & (s->ringWords - 1u);

      if (word & 0x100u) {
        if (readIdx == 0) {
          s->inst->inputData((uint8_t *)&s->ringBuf[dataStart],
                             (s->ringWords - dataStart), sizeof(uint32_t));
          dataStart = 0;
          s->readIdx = readIdx;
        }
      } else {
        uint32_t dataLen = lastReadIdx - dataStart;
        if (dataLen != 0) {
          s->inst->inputData((uint8_t *)&s->ringBuf[dataStart], dataLen,
                             sizeof(uint32_t));
        }
        s->inst->inputCommand(static_cast<uint8_t>(word));
        dataStart = readIdx;
        s->readIdx = readIdx;
      }
    }

    uint32_t dataLen = readIdx - dataStart;
    if (dataLen != 0) {
      s->inst->inputData((uint8_t *)&s->ringBuf[dataStart], dataLen,
                         sizeof(uint32_t));
    }
    s->readIdx = readIdx;
    s->totalConsumedWords += (readIdx - iterStartIdx) & (s->ringWords - 1u);
  }
}

}  // namespace lcdtap::pico2
