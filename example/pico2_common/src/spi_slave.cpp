#include <initializer_list>

#include "lcdtap/pico2/spi_slave.hpp"

#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include "spi_4line_mode0.pio.h"

namespace lcdtap::pico2 {

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

  dma_channel_configure((uint)s->dmaCh, &cfg, s->ringBuf,
                        &s->cfg.pio->rxf[s->cfg.sm], 0xFFFFFFFFu,
                        /*trigger=*/true);
}

void spiSlaveRegisterIrq(SpiSlaveState *s) {
  gpio_set_irq_enabled(s->cfg.pinCs, GPIO_IRQ_EDGE_RISE, true);
}

void spiSlaveDeinit(SpiSlaveState *s) {
  gpio_set_irq_enabled(s->cfg.pinCs, GPIO_IRQ_EDGE_RISE, false);
  if (s->dmaCh >= 0) {
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
  uint32_t writeAddr = dma_channel_hw_addr((uint)s->dmaCh)->write_addr;
  uint32_t writeIdx =
      (writeAddr - reinterpret_cast<uint32_t>(s->ringBuf)) / sizeof(uint32_t);
  writeIdx &= (s->ringWords - 1u);

  if (!s->inst) {
    s->readIdx = writeIdx;
    return;
  }

  uint32_t dataStart = s->readIdx;
  while (s->readIdx != writeIdx) {
    uint32_t lastReadIdx = s->readIdx;
    uint32_t word = s->ringBuf[s->readIdx];
    s->readIdx = (s->readIdx + 1u) & (s->ringWords - 1u);

    if (word & 0x100u) {
      if (s->readIdx == 0) {
        s->inst->inputData((uint8_t *)&s->ringBuf[dataStart],
                           (s->ringWords - dataStart), sizeof(uint32_t));
        dataStart = 0;
      }
    } else {
      uint32_t dataLen = lastReadIdx - dataStart;
      if (dataLen != 0) {
        s->inst->inputData((uint8_t *)&s->ringBuf[dataStart], dataLen,
                           sizeof(uint32_t));
      }
      s->inst->inputCommand(static_cast<uint8_t>(word));
      dataStart = s->readIdx;
    }
  }

  uint32_t dataLen = s->readIdx - dataStart;
  if (dataLen != 0) {
    s->inst->inputData((uint8_t *)&s->ringBuf[dataStart], dataLen,
                       sizeof(uint32_t));
  }
}

}  // namespace pico2
