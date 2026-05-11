#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/regs/i2c.h"
#include "hardware/structs/i2c.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "common_dvi_pin_configs.h"  // pico_sock_cfg
#include "dvi.h"
#include "dvi_timing.h"

#include "spilcd2dvi/spilcd2dvi.hpp"

#include "config.h"
#include "par_slave.pio.h"

// =============================================================================
// Memory pool (bump allocator for SpiLcd2Dvi)
// =============================================================================
static uint8_t memPool[MEM_POOL_SIZE];
static size_t memPoolOffset = 0;

static void *poolAlloc(size_t size) {
  size = (size + 3u) & ~3u;
  if (memPoolOffset + size > sizeof(memPool)) return nullptr;
  void *p = memPool + memPoolOffset;
  memPoolOffset += size;
  return p;
}

// =============================================================================
// SPI ring buffer (SPI mode)
// word = [bit8: DCX, bits7:0: data byte]
// =============================================================================
static uint32_t
    __attribute__((aligned(SPI_RING_BUF_BYTES))) spiRingBuf[SPI_RING_BUF_WORDS];

static int spiDmaCh = -1;
static uint32_t spiReadIdx = 0;

// =============================================================================
// I2C ring buffer (I2C mode)
// Filled by IRQ handler; drained by main loop.
// Same word format: bit[8]=DCX, bits[7:0]=data byte.
// =============================================================================
static uint32_t i2cRingBuf[I2C_RING_BUF_WORDS];
static volatile uint32_t i2cWriteIdx = 0;  // written by IRQ
static uint32_t i2cReadIdx = 0;            // read by main loop

// I2C control-byte state machine
enum class I2cRxState { WAIT_CTRL, STREAM_CMD, STREAM_DATA };
static volatile I2cRxState i2cRxState = I2cRxState::WAIT_CTRL;

// =============================================================================
// DVI
// =============================================================================
static struct dvi_inst dvi0;
static uint16_t *scanlineBufs[N_SCANLINE_BUFS];

// =============================================================================
// SpiLcd2Dvi instance
// =============================================================================
static sl2d::SpiLcd2Dvi *gSl2d = nullptr;

// =============================================================================
// GPIO interrupt handler (RESX pin, SPI mode)
// =============================================================================
static void gpioIrqHandler(uint gpio, uint32_t events) {
  if (gpio == PIN_PAR_RESX && gSl2d) {
    gSl2d->inputReset((events & GPIO_IRQ_EDGE_FALL) != 0u);
  }
}

// =============================================================================
// Core 1: DVI TMDS encode + serialise (never returns)
// =============================================================================
static void core1Main() {
  dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
  while (queue_is_empty(&dvi0.q_colour_valid)) __wfe();
  dvi_start(&dvi0);
  dvi_scanbuf_main_16bpp(&dvi0);
}

// =============================================================================
// Parallel slave init (SPI mode, PIO1 SM0)
// =============================================================================
static void parSlaveInit(uint prog_offset) {
  gpio_init(PIN_PAR_BCLK);
  gpio_set_dir(PIN_PAR_BCLK, GPIO_IN);
  gpio_init(PIN_PAR_DCX);
  gpio_set_dir(PIN_PAR_DCX, GPIO_IN);
  for (uint pin = PIN_PAR_DATA_BASE; pin < PIN_PAR_DATA_BASE + 8u; ++pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
  }
  gpio_pull_up(PIN_PAR_RESX);

  pio_sm_config c = par_slave_with_dcx_program_get_default_config(prog_offset);
  sm_config_set_in_pins(&c, PIN_PAR_DATA_BASE);
  sm_config_set_jmp_pin(&c, PIN_PAR_DCX);
  sm_config_set_in_shift(&c, false, false, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

  pio_sm_init(SPI_PIO, SPI_SM, prog_offset, &c);
  pio_sm_set_enabled(SPI_PIO, SPI_SM, true);
}

// =============================================================================
// DMA init (SPI mode, call AFTER dvi_init)
// =============================================================================
static void spiDmaInit() {
  spiDmaCh = dma_claim_unused_channel(true);

  dma_channel_config cfg = dma_channel_get_default_config((uint)spiDmaCh);
  channel_config_set_read_increment(&cfg, false);
  channel_config_set_write_increment(&cfg, true);
  channel_config_set_dreq(&cfg, pio_get_dreq(SPI_PIO, SPI_SM, false));
  channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
  channel_config_set_ring(&cfg, true, SPI_RING_BUF_LOG2);

  dma_channel_configure((uint)spiDmaCh, &cfg, spiRingBuf, &SPI_PIO->rxf[SPI_SM],
                        0xFFFFFFFFu, true);
}

// =============================================================================
// I2C slave init (I2C mode)
// =============================================================================

static void i2cSlaveIrqHandler() {
  i2c_hw_t *hw = i2c_get_hw(i2c0);
  uint32_t intr = hw->intr_stat;

  if (intr & I2C_IC_INTR_STAT_R_RX_FULL_BITS) {
    uint32_t raw = hw->data_cmd;
    bool firstByte = (raw & I2C_IC_DATA_CMD_FIRST_DATA_BYTE_BITS) != 0u;
    uint8_t byte = static_cast<uint8_t>(raw & 0xFFu);

    if (firstByte) {
      // First byte after address phase is always the SSD1309 control byte.
      i2cRxState = I2cRxState::WAIT_CTRL;
    }

    if (i2cRxState == I2cRxState::WAIT_CTRL) {
      // Decode SSD1309 control byte: bit6=D/C#, bit7=Co (Co=1 not supported)
      bool dc = (byte >> 6u) & 1u;
      i2cRxState = dc ? I2cRxState::STREAM_DATA : I2cRxState::STREAM_CMD;
    } else {
      // Command or GDDRAM data — push to ring buffer
      uint32_t word = (i2cRxState == I2cRxState::STREAM_DATA)
                          ? (0x100u | byte)
                          : static_cast<uint32_t>(byte);
      i2cRingBuf[i2cWriteIdx & (I2C_RING_BUF_WORDS - 1u)] = word;
      i2cWriteIdx++;
    }
  }

  if (intr & I2C_IC_INTR_STAT_R_STOP_DET_BITS) {
    i2cRxState = I2cRxState::WAIT_CTRL;
    (void)hw->clr_stop_det;
  }

  // Respond to read requests with a dummy byte (read-back not supported)
  if (intr & I2C_IC_INTR_STAT_R_RD_REQ_BITS) {
    hw->data_cmd = 0xFFu;
    (void)hw->clr_rd_req;
  }

  if (intr & I2C_IC_INTR_STAT_R_TX_ABRT_BITS) {
    (void)hw->clr_tx_abrt;
  }
}

static void i2cSlaveInit() {
  i2c_init(i2c0, 400u * 1000u);  // baudrate irrelevant in slave mode
  gpio_set_function(PIN_I2C_SDA, GPIO_FUNC_I2C);
  gpio_set_function(PIN_I2C_SCL, GPIO_FUNC_I2C);
  gpio_pull_up(PIN_I2C_SDA);
  gpio_pull_up(PIN_I2C_SCL);

  i2c_set_slave_mode(i2c0, true, I2C_SLAVE_ADDR);

  // Enable RX_FULL, STOP_DET, RD_REQ, TX_ABRT interrupts
  i2c_get_hw(i2c0)->intr_mask =
      I2C_IC_INTR_MASK_M_RX_FULL_BITS | I2C_IC_INTR_MASK_M_STOP_DET_BITS |
      I2C_IC_INTR_MASK_M_RD_REQ_BITS | I2C_IC_INTR_MASK_M_TX_ABRT_BITS;

  irq_set_exclusive_handler(I2C0_IRQ, i2cSlaveIrqHandler);
  irq_set_enabled(I2C0_IRQ, true);
}

// =============================================================================
// Drain SPI ring buffer and feed to SpiLcd2Dvi
// =============================================================================
static uint8_t dataBatch[DATA_BATCH_CAP];
static size_t dataBatchLen = 0;

static void flushDataBatch() {
  if (dataBatchLen != 0) {
    gSl2d->inputData(dataBatch, dataBatchLen);
    dataBatchLen = 0;
  }
}

static inline void processWord(uint32_t word) {
  if (word & 0x100u) {
    dataBatch[dataBatchLen++] = static_cast<uint8_t>(word);
    if (dataBatchLen >= DATA_BATCH_CAP) flushDataBatch();
  } else {
    flushDataBatch();
    gSl2d->inputCommand(static_cast<uint8_t>(word));
  }
}

static void processSpiRingBuf() {
  uint32_t writeAddr = dma_channel_hw_addr((uint)spiDmaCh)->write_addr;
  uint32_t writeIdx =
      (writeAddr - reinterpret_cast<uint32_t>(spiRingBuf)) / sizeof(uint32_t);
  writeIdx &= (SPI_RING_BUF_WORDS - 1u);

  if (!gSl2d) {
    spiReadIdx = writeIdx;
    return;
  }
  while (spiReadIdx != writeIdx) {
    processWord(spiRingBuf[spiReadIdx]);
    spiReadIdx = (spiReadIdx + 1u) & (SPI_RING_BUF_WORDS - 1u);
  }
}

static void processI2cRingBuf() {
  if (!gSl2d) return;
  uint32_t writeIdx = i2cWriteIdx;  // snapshot of volatile
  while (i2cReadIdx != writeIdx) {
    processWord(i2cRingBuf[i2cReadIdx & (I2C_RING_BUF_WORDS - 1u)]);
    ++i2cReadIdx;
  }
}

// =============================================================================
// main
// =============================================================================
int main() {
  // -------------------------------------------------------------------------
  // 1. Read boot-time configuration GPIOs
  // -------------------------------------------------------------------------
  for (uint pin : {PIN_CFG_INPUT_MODE, PIN_CFG_DVI_RES, PIN_CFG_SCALE_MODE0,
                   PIN_CFG_SCALE_MODE1}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_down(pin);
  }
  sleep_ms(1);

  const bool useI2C = gpio_get(PIN_CFG_INPUT_MODE);
  const bool dvi720p = gpio_get(PIN_CFG_DVI_RES);
  const uint8_t scale = static_cast<uint8_t>(
      (gpio_get(PIN_CFG_SCALE_MODE1) << 1u) | gpio_get(PIN_CFG_SCALE_MODE0));

  const struct dvi_timing *timing =
      dvi720p ? &dvi_timing_1280x720p_reduced_30hz : &dvi_timing_640x480p_60hz;

  sl2d::ScaleMode scaleMode;
  switch (scale & 0x3u) {
    case 1u: scaleMode = sl2d::ScaleMode::FIT; break;
    case 2u: scaleMode = sl2d::ScaleMode::PIXEL_PERFECT; break;
    default: scaleMode = sl2d::ScaleMode::STRETCH; break;
  }

  // -------------------------------------------------------------------------
  // 2. Voltage regulator and system clock
  // -------------------------------------------------------------------------
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  sleep_ms(10);
  set_sys_clock_khz(timing->bit_clk_khz, true);

  // -------------------------------------------------------------------------
  // 3. LED
  // -------------------------------------------------------------------------
  gpio_init(PIN_LED);
  gpio_set_dir(PIN_LED, GPIO_OUT);
  gpio_put(PIN_LED, 0);

  // -------------------------------------------------------------------------
  // 4. DVI init
  // -------------------------------------------------------------------------
  dvi0.timing = timing;
  dvi0.ser_cfg = pico_sock_cfg;
  dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

  const uint32_t dviW = timing->h_active_pixels / 2;
  const uint32_t dviH = timing->v_active_lines / DVI_VERTICAL_REPEAT;

  for (int i = 0; i < N_SCANLINE_BUFS; ++i) {
    scanlineBufs[i] =
        static_cast<uint16_t *>(poolAlloc(dviW * sizeof(uint16_t)));
    if (!scanlineBufs[i]) panic("scanline buf alloc failed");
    queue_add_blocking_u32(&dvi0.q_colour_free, &scanlineBufs[i]);
  }

  // -------------------------------------------------------------------------
  // 5. SpiLcd2Dvi init (SSD1309, 128x64 fixed)
  // -------------------------------------------------------------------------
  sl2d::Sl2dConfig sl2dCfg;
  sl2d::getDefaultConfig(sl2d::Controller::SSD1309, &sl2dCfg);
  sl2dCfg.scaleMode = scaleMode;
  sl2dCfg.invertInvPolarity = false;  // fixed
  sl2dCfg.dviWidth = static_cast<uint16_t>(dviW);
  sl2dCfg.dviHeight = static_cast<uint16_t>(dviH);

  sl2d::HostInterface host;
  host.alloc = poolAlloc;
  host.free = [](void *) {};
  host.log = nullptr;
  host.userData = nullptr;

  sl2d::SpiLcd2Dvi sl2dInst(sl2dCfg, host);
  if (sl2dInst.getStatus() != sl2d::Status::OK) panic("SpiLcd2Dvi init failed");

  // Checkerboard test pattern (8x8 blocks)
  {
    uint16_t *fb = sl2dInst.getFramebuf();
    for (uint16_t y = 0; y < sl2dCfg.lcdHeight; ++y) {
      for (uint16_t x = 0; x < sl2dCfg.lcdWidth; ++x) {
        fb[(uint32_t)y * sl2dCfg.lcdWidth + x] =
            (((x / 8u) ^ (y / 8u)) & 1u) ? 0xFFFFu : 0x0000u;
      }
    }
    sl2dInst.setDisplayOn(true);
  }

  gSl2d = &sl2dInst;

  // -------------------------------------------------------------------------
  // 6. Input peripheral init
  // -------------------------------------------------------------------------
  if (useI2C) {
    i2cSlaveInit();
  } else {
    // SPI mode: RESX GPIO interrupt
    gpio_init(PIN_PAR_RESX);
    gpio_set_dir(PIN_PAR_RESX, GPIO_IN);
    gpio_pull_up(PIN_PAR_RESX);
    gpio_set_irq_enabled_with_callback(PIN_PAR_RESX,
                                       GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                       true, &gpioIrqHandler);

    // Parallel slave PIO + DMA (after dvi_init so DVI claims its channels
    // first)
    uint pioProgOffset = pio_add_program(SPI_PIO, &par_slave_with_dcx_program);
    parSlaveInit(pioProgOffset);
    spiDmaInit();
  }

  // -------------------------------------------------------------------------
  // 7. Launch Core 1 for DVI output
  // -------------------------------------------------------------------------
  multicore_launch_core1(core1Main);

  // -------------------------------------------------------------------------
  // 8. Main loop (Core 0)
  // -------------------------------------------------------------------------
  uint32_t scanY = 0;
  uint32_t frame = 0;
  bool led = false;

  while (true) {
    uint16_t *buf;
    while (!queue_try_remove_u32(&dvi0.q_colour_free, &buf)) {
      if (useI2C)
        processI2cRingBuf();
      else
        processSpiRingBuf();
    }

    gSl2d->fillScanline(static_cast<uint16_t>(scanY), buf);

    queue_add_blocking_u32(&dvi0.q_colour_valid, &buf);

    if (useI2C)
      processI2cRingBuf();
    else
      processSpiRingBuf();

    if (++scanY >= dviH) {
      scanY = 0;
      if (++frame % LED_TOGGLE_FRAMES == 0u) {
        led = !led;
        gpio_put(PIN_LED, led ? 1 : 0);
      }
    }
  }
}
