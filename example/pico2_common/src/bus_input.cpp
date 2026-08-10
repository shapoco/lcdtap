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
  // 6800-style hosts (ST7032 E) latch on the falling edge; invert the pad
  // input so the rising-edge 8080 PIO program samples at the right moment.
  gpio_set_inover(ctx.pinParWr, ctx.parWrInvert ? GPIO_OVERRIDE_INVERT
                                                : GPIO_OVERRIDE_NORMAL);
  for (uint pin = ctx.pinParDataBase; pin < ctx.pinParDataBase + 8u; ++pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    // 4-bit-wired hosts drive only D[7:4]; pull the unused low nibble to 0.
    if (ctx.parWrInvert && pin < ctx.pinParDataBase + 4u) {
      gpio_pull_down(pin);
    }
  }
  gpio_init(ctx.pinParDataBase + 8u);  // D/C#
  gpio_set_dir(ctx.pinParDataBase + 8u, GPIO_IN);

  pio_sm_config c = parallel_8bit_program_get_default_config(progOffset);
  sm_config_set_in_pins(&c, ctx.pinParDataBase);
  sm_config_set_in_shift(&c, /*shift_direction=*/false, /*autopush=*/false,
                         /*push_threshold=*/32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  // CS (8080) / R/W (6800): the SM samples this pin at each strobe via
  // JMP PIN and discards the capture while it is high, filtering host READ
  // cycles (busy-flag polling) race-free (issue 0014).
  sm_config_set_jmp_pin(&c, cfg.pinCs);

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

  // Undo any parallel-mode pad overrides before the new interface claims the
  // pins (WR# shares the SPI SCLK pin). parSlaveInit re-applies them.
  gpio_set_inover(ctx.pinParWr, GPIO_OVERRIDE_NORMAL);
  for (uint pin = ctx.pinParDataBase; pin < ctx.pinParDataBase + 4u; ++pin) {
    gpio_disable_pulls(pin);
  }

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
      // No CS-rise IRQ here: the PIO program gates every capture on the
      // CS / R/W level itself (issue 0014). The IRQ-based SM parking lost
      // the race against read strobes under cross-core XIP contention.
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
