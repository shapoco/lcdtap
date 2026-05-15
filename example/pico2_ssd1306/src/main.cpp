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

#include "lcdtap/lcdtap.hpp"

#include "config.h"
#include "spi_slave.pio.h"

static_assert(PIN_SPI_CS == SPI_CS_PIN,
              "PIN_SPI_CS mismatch with spi_slave.pio");
static_assert(PIN_SPI_SCLK == SPI_SCLK_PIN,
              "PIN_SPI_SCLK mismatch with spi_slave.pio");
static_assert(PIN_SPI_MOSI + 1u == PIN_SPI_DCX,
              "DCX must be MOSI+1 for 'in pins, 2'");

// =============================================================================
// Memory pool (bump allocator for LcdTap)
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
// LcdTap instance
// =============================================================================
static lcdtap::LcdTap *gInst = nullptr;

// PIO program offset — stored so gpioIrqHandler can reset the SM PC on CS
// de-assertion (pio_sm_restart does not reset the PC).
static uint gSpiProgOffset = 0u;

// =============================================================================
// GPIO interrupt handler (RESX and CS pins, SPI mode)
// =============================================================================
static void gpioIrqHandler(uint gpio, uint32_t events) {
  if (gpio == PIN_SPI_RESX && gInst) {
    gInst->inputReset((events & GPIO_IRQ_EDGE_FALL) != 0u);
  }
  if (gpio == PIN_SPI_CS && (events & GPIO_IRQ_EDGE_RISE)) {
    // CS rising edge: transaction ended or aborted.  Reset the SM so any
    // partial byte is discarded and it is ready for the next transaction.
    pio_sm_set_enabled(SPI_PIO, SPI_SM, false);
    pio_sm_clear_fifos(SPI_PIO, SPI_SM);
    pio_sm_restart(SPI_PIO, SPI_SM);
    pio_sm_exec(SPI_PIO, SPI_SM, pio_encode_jmp(gSpiProgOffset));
    pio_sm_set_enabled(SPI_PIO, SPI_SM, true);
    // SM resumes from .wrap_target: 'wait 0 gpio SPI_CS_PIN'.
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
// SPI slave init (SPI mode, PIO1 SM0)
// =============================================================================
static void spiSlaveInit(uint prog_offset) {
  for (uint pin : {PIN_SPI_SCLK, PIN_SPI_MOSI, PIN_SPI_DCX}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
  }
  gpio_init(PIN_SPI_CS);
  gpio_set_dir(PIN_SPI_CS, GPIO_IN);
  gpio_pull_up(PIN_SPI_CS);

  pio_sm_config c = spi_slave_with_dcx_program_get_default_config(prog_offset);
  sm_config_set_in_pins(&c, PIN_SPI_MOSI);  // IN_BASE=GPIO4; DCX is IN_BASE+1
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
      // First byte after address phase is always the SSD1306 control byte.
      i2cRxState = I2cRxState::WAIT_CTRL;
    }

    if (i2cRxState == I2cRxState::WAIT_CTRL) {
      // Decode SSD1306 control byte: bit6=D/C#, bit7=Co (Co=1 not supported)
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
// Drain SPI ring buffer and feed to LcdTap
// =============================================================================
static uint8_t dataBatch[DATA_BATCH_CAP];
static size_t dataBatchLen = 0;

static void flushDataBatch() {
  if (dataBatchLen != 0) {
    gInst->inputData(dataBatch, dataBatchLen);
    dataBatchLen = 0;
  }
}

static inline void processWord(uint32_t word) {
  if (word & 0x100u) {
    dataBatch[dataBatchLen++] = static_cast<uint8_t>(word);
    if (dataBatchLen >= DATA_BATCH_CAP) flushDataBatch();
  } else {
    flushDataBatch();
    gInst->inputCommand(static_cast<uint8_t>(word));
  }
}

static void processSpiRingBuf() {
  uint32_t writeAddr = dma_channel_hw_addr((uint)spiDmaCh)->write_addr;
  uint32_t writeIdx =
      (writeAddr - reinterpret_cast<uint32_t>(spiRingBuf)) / sizeof(uint32_t);
  writeIdx &= (SPI_RING_BUF_WORDS - 1u);

  if (!gInst) {
    spiReadIdx = writeIdx;
    return;
  }
  while (spiReadIdx != writeIdx) {
    processWord(spiRingBuf[spiReadIdx]);
    spiReadIdx = (spiReadIdx + 1u) & (SPI_RING_BUF_WORDS - 1u);
  }
}

static void processI2cRingBuf() {
  if (!gInst) return;
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
  for (uint pin :
       {PIN_CFG_INPUT_MODE, PIN_CFG_DVI_RES, PIN_CFG_ROT0, PIN_CFG_ROT1}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_down(pin);
  }
  sleep_ms(1);

  const bool useI2C = !gpio_get(PIN_CFG_INPUT_MODE);
  const bool dvi720p = gpio_get(PIN_CFG_DVI_RES);
  const int rot =
      static_cast<int>((gpio_get(PIN_CFG_ROT1) << 1u) | gpio_get(PIN_CFG_ROT0));

  const struct dvi_timing *timing =
      dvi720p ? &dvi_timing_1280x720p_reduced_30hz : &dvi_timing_640x480p_60hz;

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
  // 5. LcdTap init (SSD1306)
  // -------------------------------------------------------------------------
  lcdtap::LcdTapConfig cfg;
  lcdtap::getDefaultConfig(lcdtap::ControllerType::SSD1306, &cfg);
  cfg.lcdWidth = LCDTAP_LCD_SIZE_W;
  cfg.lcdHeight = LCDTAP_LCD_SIZE_H;
  cfg.scaleMode = lcdtap::ScaleMode::FIT;
  cfg.invertInvPolarity = false;  // fixed
  cfg.dviWidth = static_cast<uint16_t>(dviW);
  cfg.dviHeight = static_cast<uint16_t>(dviH);

  lcdtap::HostInterface host;
  host.alloc = poolAlloc;
  host.free = [](void *) {};
  host.log = nullptr;
  host.userData = nullptr;

  lcdtap::LcdTap inst(cfg, host);
  if (inst.getStatus() != lcdtap::Status::OK) panic("LcdTap init failed");

  // Checkerboard test pattern (8x8 blocks)
  {
    uint16_t *fb = inst.getFramebuf();
    for (uint16_t y = 0; y < cfg.lcdHeight; ++y) {
      for (uint16_t x = 0; x < cfg.lcdWidth; ++x) {
        fb[(uint32_t)y * cfg.lcdWidth + x] =
            (((x / 8u) ^ (y / 8u)) & 1u) ? 0xFFFFu : 0x0000u;
      }
    }
    inst.setDisplayOn(true);
  }

  gInst = &inst;

  inst.setOutputRotation(rot);  // apply boot-time rotation setting

  // -------------------------------------------------------------------------
  // 6. Input peripheral init
  // -------------------------------------------------------------------------
  if (useI2C) {
    i2cSlaveInit();
  } else {
    // RESX: interrupt on both edges (reset assert / release)
    gpio_init(PIN_SPI_RESX);
    gpio_set_dir(PIN_SPI_RESX, GPIO_IN);
    gpio_pull_up(PIN_SPI_RESX);
    gpio_set_irq_enabled_with_callback(PIN_SPI_RESX,
                                       GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                       true, &gpioIrqHandler);
    // CS: rising edge only — resets the PIO SM on transaction end/abort.
    // Shares the callback already registered for RESX above.
    gpio_set_irq_enabled(PIN_SPI_CS, GPIO_IRQ_EDGE_RISE, true);

    // SPI slave PIO + DMA (after dvi_init so DVI claims its channels first)
    gSpiProgOffset = pio_add_program(SPI_PIO, &spi_slave_with_dcx_program);
    spiSlaveInit(gSpiProgOffset);
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
  int currentRot = rot;

  while (true) {
    uint16_t *buf;
    while (!queue_try_remove_u32(&dvi0.q_colour_free, &buf)) {
      if (useI2C)
        processI2cRingBuf();
      else
        processSpiRingBuf();
    }

    gInst->fillScanline(static_cast<uint16_t>(scanY), buf);

    queue_add_blocking_u32(&dvi0.q_colour_valid, &buf);

    if (useI2C)
      processI2cRingBuf();
    else
      processSpiRingBuf();

    if (++scanY >= dviH) {
      scanY = 0;
      // Check rotation GPIO and update if changed
      int newRot = static_cast<int>((gpio_get(PIN_CFG_ROT1) << 1u) |
                                    gpio_get(PIN_CFG_ROT0));
      if (newRot != currentRot) {
        currentRot = newRot;
        gInst->setOutputRotation(currentRot);
      }
      if (++frame % LED_TOGGLE_FRAMES == 0u) {
        led = !led;
        gpio_put(PIN_LED, led ? 1 : 0);
      }
    }
  }
}
