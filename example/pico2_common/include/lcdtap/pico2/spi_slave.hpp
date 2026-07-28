#pragma once
#include <cstdint>

#include <hardware/pio.h>
#include "lcdtap/lcdtap.hpp"
#include "spi_4line_mode0.pio.h"  // SPI_CS_PIN, SPI_SCLK_PIN

namespace lcdtap::pico2 {

struct SpiSlaveConfig {
  PIO pio;
  uint sm;
  uint pinCs;
  uint pinSclk;
  uint pinMosi;  // IN_BASE; pinDc = pinMosi + 1
  uint pinDc;
  uint32_t ringLog2;  // DMA ring buffer size (log2 bytes)
};

struct SpiSlaveState {
  // Set by caller before spiSlaveProcess calls:
  lcdtap::LcdTap *inst;
  // Internal fields:
  SpiSlaveConfig cfg;
  uint32_t *ringBuf;
  uint32_t ringWords;
  int dmaCh;
  uint progOffset;
  const pio_program_t *pioProgram;
  uint32_t readIdx;

  // Debug statistics. wrapCount/lastWriteIdx/totalReceivedWords/
  // totalConsumedWords are internal ring accounting (reset together on DMA
  // init); dropWords/backlogMaxWords are free-running 32-bit counters that
  // survive interface switches — display code resets them by baseline
  // subtraction.
  volatile uint32_t wrapCount;  // ring wraps, only used to detect full laps
  uint32_t lastWriteIdx;        // write index at the previous process call
  uint32_t totalReceivedWords;  // received words, from write pointer deltas
  uint32_t totalConsumedWords;  // words consumed by spiSlaveProcess
  uint32_t dropWords;           // words lost to ring buffer overrun
  uint32_t backlogMaxWords;     // high-water mark of the ring backlog
};

// Load spi_4line_mode0 program, configure SM, and init DMA.
// ringBuf must have __attribute__((aligned(1 << ringLog2))).
// ringWords = (1 << ringLog2) / sizeof(uint32_t).
void spiSlaveInit(SpiSlaveState *s, const SpiSlaveConfig &cfg,
                  uint32_t *ringBuf, uint32_t ringWords);

// Initialize only the DMA channel using the fields already in *s.
// Use this when the PIO program and SM are configured externally (e.g. for
// 3-line SPI or parallel modes in switchInterface). s->cfg, s->ringBuf,
// s->ringWords, and s->cfg.ringLog2 must already be set.
void spiSlaveInitDma(SpiSlaveState *s);

// Enable CS GPIO IRQ (rising edge → spiSlaveResetSm via caller's handler).
// Assumes gpio_set_irq_enabled_with_callback has already been called for this
// GPIO bank. Only one SpiSlaveState instance can be active at a time.
void spiSlaveRegisterIrq(SpiSlaveState *s);

// Disable CS IRQ, abort DMA, stop SM, and remove PIO program.
void spiSlaveDeinit(SpiSlaveState *s);

// Reset the PIO SM to program start.
// Call from the GPIO IRQ handler on CS rise or RST fall.
void spiSlaveResetSm(SpiSlaveState *s);

// Drain the DMA ring buffer and dispatch commands/data to s->inst.
void __not_in_flash_func(spiSlaveProcess)(SpiSlaveState *s);

}  // namespace lcdtap::pico2
