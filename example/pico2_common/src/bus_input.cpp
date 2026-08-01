#include "lcdtap/pico2/bus_input.hpp"

#include <initializer_list>

#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "parallel_8bit.pio.h"
#include "spi_3line_mode0.pio.h"

namespace lcdtap::pico2 {

// =============================================================================
// 3-Line SPI slave init (program already added at progOffset)
// =============================================================================
static void spi3lineSlaveInit(const SpiSlaveConfig& cfg, uint progOffset) {
  for (uint pin : {cfg.pinSclk, cfg.pinMosi}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
  }
  gpio_init(cfg.pinCs);
  gpio_set_dir(cfg.pinCs, GPIO_IN);
  gpio_pull_up(cfg.pinCs);

  pio_sm_config c = spi_3line_mode0_program_get_default_config(progOffset);
  sm_config_set_in_pins(&c, cfg.pinMosi);  // IN_BASE = MOSI only
  sm_config_set_in_shift(&c, /*shift_direction=*/false, /*autopush=*/false,
                         /*push_threshold=*/32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  sm_config_set_jmp_pin(&c, cfg.pinCs);

  pio_sm_init(cfg.pio, cfg.sm, progOffset, &c);
  pio_sm_set_enabled(cfg.pio, cfg.sm, true);
}

// =============================================================================
// Parallel slave init (program already added at progOffset)
// =============================================================================
static void parSlaveInit(const BusInputContext& ctx, uint progOffset) {
  const SpiSlaveConfig& cfg = ctx.spi->cfg;
  gpio_init(cfg.pinCs);
  gpio_set_dir(cfg.pinCs, GPIO_IN);
  gpio_pull_up(cfg.pinCs);
  gpio_init(ctx.pinParWr);
  gpio_set_dir(ctx.pinParWr, GPIO_IN);
  for (uint pin = ctx.pinParDataBase; pin < ctx.pinParDataBase + 8u; ++pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
  }
  gpio_init(ctx.pinParDataBase + 8u);  // D/C#
  gpio_set_dir(ctx.pinParDataBase + 8u, GPIO_IN);

  pio_sm_config c = parallel_8bit_program_get_default_config(progOffset);
  sm_config_set_in_pins(&c, ctx.pinParDataBase);
  sm_config_set_in_shift(&c, /*shift_direction=*/false, /*autopush=*/false,
                         /*push_threshold=*/32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

  pio_sm_init(cfg.pio, cfg.sm, progOffset, &c);
  pio_sm_set_enabled(cfg.pio, cfg.sm, true);
}

// =============================================================================
// Interface switching (teardown current, setup new)
// =============================================================================
void busInputSwitch(const BusInputContext& ctx, LcdTap* inst, BusType* current,
                    bool* active, BusType next) {
  if (*active) {
    if (*current == BusType::I2C) {
      i2cSlaveDeinit(ctx.i2c);
    } else {
      spiSlaveDeinit(ctx.spi);
    }
    if (inst) {
      inst->inputReset(true);
      inst->inputReset(false);
    }
  }
  *active = true;

  switch (next) {
    case BusType::I2C: {
      i2cSlaveInit(ctx.i2c, ctx.i2cCfg, ctx.i2cRingBuf, ctx.i2cRingWords);
      ctx.i2c->inst = inst;
      break;
    }
    case BusType::SPI_4LINE: {
      spiSlaveInit(ctx.spi, ctx.spi->cfg, ctx.spi->ringBuf, ctx.spi->ringWords);
      ctx.spi->inst = inst;
      spiSlaveRegisterIrq(ctx.spi);
      break;
    }
    case BusType::SPI_3LINE: {
      ctx.spi->progOffset =
          pio_add_program(ctx.spi->cfg.pio, &spi_3line_mode0_program);
      ctx.spi->pioProgram = &spi_3line_mode0_program;
      spi3lineSlaveInit(ctx.spi->cfg, ctx.spi->progOffset);
      ctx.spi->inst = inst;
      spiSlaveInitDma(ctx.spi);
      gpio_set_irq_enabled(ctx.spi->cfg.pinCs, GPIO_IRQ_EDGE_RISE, true);
      break;
    }
    case BusType::PARALLEL: {
      ctx.spi->progOffset =
          pio_add_program(ctx.spi->cfg.pio, &parallel_8bit_program);
      ctx.spi->pioProgram = &parallel_8bit_program;
      parSlaveInit(ctx, ctx.spi->progOffset);
      ctx.spi->inst = inst;
      spiSlaveInitDma(ctx.spi);
      gpio_set_irq_enabled(ctx.spi->cfg.pinCs, GPIO_IRQ_EDGE_RISE, true);
      break;
    }
  }
  *current = next;
}

// =============================================================================
// Dispatch to the active ring buffer processor
// =============================================================================
void busInputProcess(const BusInputContext& ctx, BusType current) {
  if (current == BusType::I2C) {
    i2cSlaveProcess(ctx.i2c);
  } else {
    spiSlaveProcess(ctx.spi);
  }
}

}  // namespace lcdtap::pico2
