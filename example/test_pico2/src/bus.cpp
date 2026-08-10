#include "testrig/bus.hpp"

#include <cstring>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "par8_master.pio.h"
#include "pico/stdlib.h"
#include "spi_master.pio.h"
#include "testrig/config.h"

namespace testrig {

namespace {

// PIO-USB owns pio0/pio1 (TX/RX); the bus masters live on pio2.
PIO const BUS_PIO = pio2;
constexpr uint BUS_SM = 0;

i2c_inst_t* const BUS_I2C = i2c1;

lcdtap::BusType gMode = lcdtap::BusType::NUM_BUSES;
uint8_t gI2cAddr = 0x3C;
uint gProgramOffset = 0;
const pio_program_t* gLoadedProgram = nullptr;
int gDmaCh = -1;

// All target-facing pins that the inactive buses must not drive.
const uint BUS_PINS[] = {PIN_TGT_CS,     PIN_TGT_WR_SCLK, PIN_TGT_D0,
                         PIN_TGT_D0 + 1, PIN_TGT_D0 + 2,  PIN_TGT_D0 + 3,
                         PIN_TGT_D0 + 4, PIN_TGT_D0 + 5,  PIN_TGT_D0 + 6,
                         PIN_TGT_D0 + 7, PIN_TGT_PAR_DC};

void allBusPinsHiZ() {
  for (uint pin : BUS_PINS) {
    gpio_set_outover(pin, GPIO_OVERRIDE_NORMAL);
    gpio_init(pin);  // SIO input, no pulls
    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);
  }
}

void unloadProgram() {
  if (gLoadedProgram == nullptr) return;
  pio_sm_set_enabled(BUS_PIO, BUS_SM, false);
  // Remove only our program: the CYW43 PIO SPI may share this PIO block.
  pio_remove_program(BUS_PIO, gLoadedProgram, gProgramOffset);
  gLoadedProgram = nullptr;
}

// Wait until the SM has actually clocked out everything queued.
void pioDrain() {
  while (!pio_sm_is_tx_fifo_empty(BUS_PIO, BUS_SM)) tight_loop_contents();
  BUS_PIO->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + BUS_SM);
  while ((BUS_PIO->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + BUS_SM))) == 0) {
    tight_loop_contents();
  }
}

void pioPutByte(uint8_t b) {
  // SPI shifts left (MSB first): byte must sit in bits 31:24. The parallel
  // program shifts right and takes bits 7:0. Writing the byte to both
  // positions serves either program.
  uint32_t v = (static_cast<uint32_t>(b) << 24) | b;
  while (pio_sm_is_tx_fifo_full(BUS_PIO, BUS_SM)) tight_loop_contents();
  BUS_PIO->txf[BUS_SM] = v;
}

void pioWriteDma(const uint8_t* data, size_t len) {
  dma_channel_config c = dma_channel_get_default_config(gDmaCh);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
  channel_config_set_read_increment(&c, true);
  channel_config_set_write_increment(&c, false);
  channel_config_set_dreq(&c, pio_get_dreq(BUS_PIO, BUS_SM, true));
  // Narrow DMA writes are replicated across the 32-bit bus, which feeds
  // both the shift-left (SPI) and shift-right (parallel) programs.
  dma_channel_configure(gDmaCh, &c, &BUS_PIO->txf[BUS_SM], data, len, true);
  dma_channel_wait_for_finish_blocking(gDmaCh);
  pioDrain();
}

void setDc(bool high) {
  uint pin =
      (gMode == lcdtap::BusType::PARALLEL) ? PIN_TGT_PAR_DC : PIN_TGT_SPI_DC;
  gpio_put(pin, high);
}

// I2C control-byte framing (i2c_slave.cpp on the target): one leading
// control byte per transaction, bit6 = D/C, Co=0 streams the rest.
void i2cWriteFramed(uint8_t ctrl, const uint8_t* data, size_t len) {
  static uint8_t stage[513];
  while (len > 0) {
    size_t chunk = len < sizeof(stage) - 1 ? len : sizeof(stage) - 1;
    stage[0] = ctrl;
    memcpy(&stage[1], data, chunk);
    i2c_write_blocking(BUS_I2C, gI2cAddr, stage, chunk + 1, false);
    data += chunk;
    len -= chunk;
  }
}

}  // namespace

void busInit() {
  gpio_init(PIN_TGT_RST);
  gpio_put(PIN_TGT_RST, 1);
  gpio_set_dir(PIN_TGT_RST, GPIO_OUT);
  allBusPinsHiZ();
  // Reserve our state machine so the CYW43 driver's free-SM search cannot
  // take it.
  pio_sm_claim(BUS_PIO, BUS_SM);
  gDmaCh = dma_claim_unused_channel(true);
}

bool busSelect(lcdtap::BusType type, uint32_t freqHz, uint8_t i2cAddr,
               bool par6800) {
  busDeselect();
  gMode = type;
  gI2cAddr = i2cAddr;

  switch (type) {
    case lcdtap::BusType::SPI_4LINE: {
      // 2 cycles/bit.
      float div = static_cast<float>(clock_get_hz(clk_sys)) /
                  (2.0f * static_cast<float>(freqHz));
      gProgramOffset = pio_add_program(BUS_PIO, &spi_master_program);
      gLoadedProgram = &spi_master_program;
      spi_master_program_init(BUS_PIO, BUS_SM, gProgramOffset, PIN_TGT_D0,
                              PIN_TGT_WR_SCLK, div);
      gpio_init(PIN_TGT_SPI_DC);
      gpio_put(PIN_TGT_SPI_DC, 1);
      gpio_set_dir(PIN_TGT_SPI_DC, GPIO_OUT);
      break;
    }

    case lcdtap::BusType::PARALLEL: {
      // 3 cycles/byte.
      float div = static_cast<float>(clock_get_hz(clk_sys)) /
                  (3.0f * static_cast<float>(freqHz));
      gProgramOffset = pio_add_program(BUS_PIO, &par8_master_program);
      gLoadedProgram = &par8_master_program;
      par8_master_program_init(BUS_PIO, BUS_SM, gProgramOffset, PIN_TGT_D0,
                               PIN_TGT_WR_SCLK, div);
      if (par6800) {
        // Inverted 8080 WR# is exactly the 6800 E waveform (idle low,
        // latch on the falling edge).
        gpio_set_outover(PIN_TGT_WR_SCLK, GPIO_OVERRIDE_INVERT);
      }
      gpio_init(PIN_TGT_PAR_DC);
      gpio_put(PIN_TGT_PAR_DC, 1);
      gpio_set_dir(PIN_TGT_PAR_DC, GPIO_OUT);
      break;
    }

    case lcdtap::BusType::I2C: {
      i2c_init(BUS_I2C, freqHz);
      gpio_set_function(PIN_TGT_I2C_SDA, GPIO_FUNC_I2C);
      gpio_set_function(PIN_TGT_I2C_SCL, GPIO_FUNC_I2C);
      // The target enables its internal pull-ups too; externals are
      // recommended for the 1 MHz / 2 MHz settings.
      gpio_pull_up(PIN_TGT_I2C_SDA);
      gpio_pull_up(PIN_TGT_I2C_SCL);
      break;
    }

    default: gMode = lcdtap::BusType::NUM_BUSES; return false;
  }

  if (type != lcdtap::BusType::I2C) {
    gpio_init(PIN_TGT_CS);
    gpio_put(PIN_TGT_CS, 1);
    gpio_set_dir(PIN_TGT_CS, GPIO_OUT);
    sleep_us(10);
    gpio_put(PIN_TGT_CS, 0);  // assert for the whole test sequence
  }
  return true;
}

void busDeselect() {
  if (gMode == lcdtap::BusType::NUM_BUSES) return;
  if (gMode == lcdtap::BusType::I2C) {
    i2c_deinit(BUS_I2C);
  } else {
    pioDrain();
    // SPI modes: the CS rising edge resets the target's SM. Parallel mode:
    // the target discards strobes while CS is high (issue 0014).
    gpio_put(PIN_TGT_CS, 1);
    sleep_us(10);
  }
  unloadProgram();
  allBusPinsHiZ();
  gMode = lcdtap::BusType::NUM_BUSES;
}

void busResetPulse(uint32_t lowMs, uint32_t settleMs) {
  gpio_put(PIN_TGT_RST, 0);
  sleep_ms(lowMs);
  gpio_put(PIN_TGT_RST, 1);
  sleep_ms(settleMs);
}

void busWriteCommand(uint8_t cmd) {
  switch (gMode) {
    case lcdtap::BusType::I2C: i2cWriteFramed(0x00, &cmd, 1); break;
    case lcdtap::BusType::SPI_4LINE:
    case lcdtap::BusType::PARALLEL:
      pioDrain();
      setDc(false);
      pioPutByte(cmd);
      pioDrain();
      setDc(true);
      break;
    default: break;
  }
}

void busWriteParams(const uint8_t* params, size_t len, bool asData) {
  if (len == 0) return;
  switch (gMode) {
    case lcdtap::BusType::I2C:
      i2cWriteFramed(asData ? 0x40 : 0x00, params, len);
      break;
    case lcdtap::BusType::SPI_4LINE:
    case lcdtap::BusType::PARALLEL:
      pioDrain();
      setDc(asData);
      for (size_t i = 0; i < len; i++) pioPutByte(params[i]);
      pioDrain();
      setDc(true);
      break;
    default: break;
  }
}

void busWriteData(const uint8_t* data, size_t len) {
  if (len == 0) return;
  switch (gMode) {
    case lcdtap::BusType::I2C: i2cWriteFramed(0x40, data, len); break;
    case lcdtap::BusType::SPI_4LINE:
    case lcdtap::BusType::PARALLEL:
      setDc(true);
      pioWriteDma(data, len);
      break;
    default: break;
  }
}

}  // namespace testrig
