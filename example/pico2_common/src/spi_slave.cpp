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
// spiSlaveProcess can detect full ring laps (overruns) that the write
// pointer alone cannot distinguish from an unchanged position.
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
  s->lastWriteIdx = 0;
  s->totalReceivedWords = 0;
  s->totalConsumedWords = 0;

  // TRIGGER_SELF mode: the counter reloads to ringWords and the channel
  // re-triggers itself each time it completes, raising the channel IRQ once
  // per ring wrap. The hardware re-trigger has no dead time; incoming words
  // are additionally buffered by the joined PIO RX FIFO (8 entries).
  sWrapIrqState = s;
  // Clear any pending wrap IRQ left over from a previous use of this channel
  // so the first process call does not count a stale wrap.
  dma_hw->ints1 = 1u << (uint)s->dmaCh;
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

// Called from the CS-rise GPIO IRQ in the SPI modes; kept in RAM so the
// reset latency stays bounded even under cross-core XIP flash contention.
void __not_in_flash_func(spiSlaveResetSm)(SpiSlaveState *s) {
  pio_sm_set_enabled(s->cfg.pio, s->cfg.sm, false);
  pio_sm_clear_fifos(s->cfg.pio, s->cfg.sm);
  pio_sm_restart(s->cfg.pio, s->cfg.sm);
  pio_sm_exec(s->cfg.pio, s->cfg.sm, pio_encode_jmp(s->progOffset));
  pio_sm_set_enabled(s->cfg.pio, s->cfg.sm, true);
}

// Ring accounting shared by the process variants: tracks received words,
// detects full ring laps (overruns) and resynchronizes on one. Called once
// per process call, outside the data loop.
static void __not_in_flash_func(spiRingAccount)(SpiSlaveState *s,
                                                dma_channel_hw_t *dmaHw) {
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

    // Pointer-delta accumulation: exact unless a full ring lap occurred
    // between calls (which is precisely the overrun we want to detect).
    s->totalReceivedWords +=
        (writeIdxNow - s->lastWriteIdx) & (s->ringWords - 1u);
    s->lastWriteIdx = writeIdxNow;

    // Wrap-based estimate, used only to detect full laps. At a ring-wrap
    // boundary it can transiently read LOW (write_addr wraps before INTS1
    // becomes visible to the CPU) but never HIGH — the IRQ handler runs on
    // this core and clears INTS1 before incrementing wrapCount — so adopting
    // it only when ahead is safe.
    const uint32_t wrapBased = (wrap + pend) * s->ringWords + writeIdxNow;
    if ((int32_t)(wrapBased - s->totalReceivedWords) > 0) {
      s->totalReceivedWords = wrapBased;
    }

    uint32_t backlog = s->totalReceivedWords - s->totalConsumedWords;
    if (backlog & 0x80000000u) {
      // Cannot happen by construction (consumption never outruns the write
      // pointer); clamp defensively without touching any state.
      backlog = 0;
    }
    if (backlog > s->ringWords) {
      // The DMA lapped the consumer: everything currently in the ring is an
      // inconsistent mix of old and new bytes. Count the overshoot (a lower
      // bound when wrap IRQs were suppressed, e.g. during a flash write) and
      // resynchronize to the write pointer.
      s->dropWords += backlog - s->ringWords;
      s->readIdx = writeIdxNow;
      s->totalConsumedWords = s->totalReceivedWords;
      backlog = 0;
    }
    if (backlog > s->backlogMaxWords) s->backlogMaxWords = backlog;
  }
}

void __not_in_flash_func(spiSlaveProcess)(SpiSlaveState *s) {
  dma_channel_hw_t *dmaHw = dma_channel_hw_addr((uint)s->dmaCh);
  spiRingAccount(s, dmaHw);

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

// Ring consumer for the dual-chip-select parallel capture (parallel_2cs.pio).
// Word layout: bits[7:0] = data byte, bit8 = D/I (1 = data), bits[30:29] =
// CS2:CS1 (high active); the remaining bits are pin readback garbage.
// Consecutive data words are batched into one zero-copy inputData() call as
// long as their cs mask stays the same.
void __not_in_flash_func(spiSlaveProcess2Cs)(SpiSlaveState *s) {
  dma_channel_hw_t *dmaHw = dma_channel_hw_addr((uint)s->dmaCh);
  spiRingAccount(s, dmaHw);

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
    uint32_t dataStart = 0;  // only meaningful while dataCs != 0
    uint8_t dataCs = 0;      // cs of the pending data batch; 0 = none
    while (readIdx != writeIdx) {
      const uint32_t lastReadIdx = readIdx;
      const uint32_t word = s->ringBuf[readIdx];
      readIdx = (readIdx + 1u) & (s->ringWords - 1u);
      const uint8_t cs = (word >> 29) & 3u;
      const bool isData = (word & 0x100u) != 0u;

      // The pending batch ends when this word cannot extend it.
      if (dataCs != 0u && (!isData || cs != dataCs)) {
        s->inst->inputData((uint8_t *)&s->ringBuf[dataStart],
                           lastReadIdx - dataStart, sizeof(uint32_t), dataCs);
        dataCs = 0u;
        s->readIdx = lastReadIdx;
      }

      if (!isData) {
        if (cs != 0u) s->inst->inputCommand(static_cast<uint8_t>(word), cs);
        s->readIdx = readIdx;
      } else if (cs == 0u) {
        // Strobe with no chip selected: discard.
        s->readIdx = readIdx;
      } else {
        if (dataCs == 0u) {
          dataCs = cs;
          dataStart = lastReadIdx;
        }
        if (readIdx == 0u) {
          // Ring wrap: flush the batch including this word.
          s->inst->inputData((uint8_t *)&s->ringBuf[dataStart],
                             s->ringWords - dataStart, sizeof(uint32_t),
                             dataCs);
          dataCs = 0u;
          s->readIdx = 0u;
        }
      }
    }

    if (dataCs != 0u && readIdx != dataStart) {
      s->inst->inputData((uint8_t *)&s->ringBuf[dataStart], readIdx - dataStart,
                         sizeof(uint32_t), dataCs);
    }
    s->readIdx = readIdx;
    s->totalConsumedWords += (readIdx - iterStartIdx) & (s->ringWords - 1u);
  }
}

}  // namespace lcdtap::pico2
