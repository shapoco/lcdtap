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
#include "lcdtap/osd.hpp"

#include "config.h"
#include "flash_config.hpp"
#include "parallel_8bit.pio.h"
#include "spi_3line_mode0.pio.h"
#include "spi_4line_mode0.pio.h"

static_assert(PIN_SPI_CS == SPI_CS_PIN,
              "PIN_SPI_CS mismatch with spi_4line_mode0.pio");
static_assert(PIN_SPI_SCLK == SPI_SCLK_PIN,
              "PIN_SPI_SCLK mismatch with spi_4line_mode0.pio");
static_assert(PIN_SPI_MOSI + 1u == PIN_SPI_DCX,
              "DCX must be MOSI+1 for 'in pins, 2'");

// =============================================================================
// Memory pool — used exclusively for the LcdTap framebuffer.
// Always returns the pool start; safe because only one allocation exists at
// a time. poolFree is a no-op: the pool is reused on the next alloc call.
// =============================================================================
static uint8_t memPool[MEM_POOL_SIZE];

static void *poolAlloc(size_t size) {
  size = (size + 3u) & ~3u;
  if (size > sizeof(memPool)) return nullptr;
  return memPool;
}

static void poolFree(void *ptr) { (void)ptr; }

// =============================================================================
// SPI/Parallel ring buffer  (word = [bit8: DCX, bits7:0: data byte])
// =============================================================================
static uint32_t
    __attribute__((aligned(SPI_RING_BUF_BYTES))) spiRingBuf[SPI_RING_BUF_WORDS];

static int spiDmaCh = -1;
static uint32_t spiReadIdx = 0;

// =============================================================================
// I2C ring buffer  (same word format as SPI ring buffer)
// =============================================================================
static uint32_t i2cRingBuf[I2C_RING_BUF_WORDS];
static volatile uint32_t i2cWriteIdx = 0;  // bounded: 0..I2C_RING_BUF_WORDS-1
static uint32_t i2cReadIdx = 0;

enum class I2cRxState { WAIT_CTRL, STREAM_CMD, STREAM_DATA };
static volatile I2cRxState i2cRxState = I2cRxState::WAIT_CTRL;

// =============================================================================
// Interface selection
// =============================================================================
static InterfaceType gCurrentIface = InterfaceType::SPI_4LINE;
static bool gIfaceActive = false;
static const pio_program_t *gCurrentPioProgram = nullptr;

// =============================================================================
// DVI
// =============================================================================
static struct dvi_inst dvi0;

static uint16_t scanlineBufs[N_SCANLINE_BUFS][DVI_MAX_W];

// =============================================================================
// LcdTap instance
// =============================================================================
static lcdtap::LcdTap *gInst = nullptr;

// OSD instance for runtime configuration
static lcdtap::Osd gOsd;

// PIO program offset — stored so gpioIrqHandler can reset the SM PC on CS
// de-assertion (pio_sm_restart does not reset the PC).
static uint gSpiProgOffset = 0u;

// =============================================================================
// OSD user item
// =============================================================================
static constexpr uint16_t OSD_USER_ITEM_ID_INTERFACE =
    lcdtap::OSD_USER_ITEM_ID_BASE;

static const char *kInterfaceNames[] = {"I2C", "4Line SPI", "3Line SPI",
                                        "Parallel"};

static void onOsdMenuOpen(lcdtap::Osd *osd, void * /*userData*/) {
  lcdtap::OsdMenuItem item = {};
  item.id = OSD_USER_ITEM_ID_INTERFACE;
  item.type = lcdtap::OsdMenuType::ENUM;
  item.name = "Interface";
  item.unit = "";
  item.values = kInterfaceNames;
  item.min = 0;
  item.max = 3;
  item.step = 1;
  item.value = static_cast<int16_t>(gCurrentIface);
  osd->insertItem(1, item);
}

// =============================================================================
// GPIO interrupt handler  (RESX pin; CS pin for SPI modes)
// =============================================================================
static void gpioIrqHandler(uint gpio, uint32_t events) {
  if (gpio == PIN_RESX && gInst) {
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
// 4-Line SPI slave init  (PIO1 SM0)
// =============================================================================
static void spiSlaveInit(uint prog_offset) {
  for (uint pin : {PIN_SPI_SCLK, PIN_SPI_MOSI, PIN_SPI_DCX}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
  }
  gpio_init(PIN_SPI_CS);
  gpio_set_dir(PIN_SPI_CS, GPIO_IN);
  gpio_pull_up(PIN_SPI_CS);

  pio_sm_config c = spi_4line_mode0_program_get_default_config(prog_offset);
  sm_config_set_in_pins(&c, PIN_SPI_MOSI);  // IN_BASE=GPIO4; DCX is IN_BASE+1
  sm_config_set_in_shift(&c, /*shift_direction=*/false, /*autopush=*/false,
                         /*push_threshold=*/32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

  pio_sm_init(SPI_PIO, SPI_SM, prog_offset, &c);
  pio_sm_set_enabled(SPI_PIO, SPI_SM, true);
}

// =============================================================================
// 3-Line SPI slave init  (PIO1 SM0)
// =============================================================================
static void spi3lineSlaveInit(uint prog_offset) {
  for (uint pin : {PIN_SPI_SCLK, PIN_SPI_MOSI}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
  }
  gpio_init(PIN_SPI_CS);
  gpio_set_dir(PIN_SPI_CS, GPIO_IN);
  gpio_pull_up(PIN_SPI_CS);

  pio_sm_config c = spi_3line_mode0_program_get_default_config(prog_offset);
  sm_config_set_in_pins(&c, PIN_SPI_MOSI);  // IN_BASE=GPIO4 (MOSI only)
  sm_config_set_in_shift(&c, /*shift_direction=*/false, /*autopush=*/false,
                         /*push_threshold=*/32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

  pio_sm_init(SPI_PIO, SPI_SM, prog_offset, &c);
  pio_sm_set_enabled(SPI_PIO, SPI_SM, true);
}

// =============================================================================
// Parallel slave init  (PIO1 SM0)
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

  pio_sm_config c = parallel_8bit_program_get_default_config(prog_offset);
  sm_config_set_in_pins(&c, PIN_PAR_DATA_BASE);
  sm_config_set_jmp_pin(&c, PIN_PAR_DCX);
  sm_config_set_in_shift(&c, /*shift_direction=*/false, /*autopush=*/false,
                         /*push_threshold=*/32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

  pio_sm_init(SPI_PIO, SPI_SM, prog_offset, &c);
  pio_sm_set_enabled(SPI_PIO, SPI_SM, true);
}

// =============================================================================
// DMA init  (call AFTER dvi_init so DVI has already claimed its channels)
// =============================================================================
static void spiDmaInit() {
  spiDmaCh = dma_claim_unused_channel(true);

  dma_channel_config cfg = dma_channel_get_default_config((uint)spiDmaCh);
  channel_config_set_read_increment(&cfg, false);
  channel_config_set_write_increment(&cfg, true);
  channel_config_set_dreq(&cfg, pio_get_dreq(SPI_PIO, SPI_SM, /*is_tx=*/false));
  channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
  channel_config_set_ring(&cfg, /*write=*/true, SPI_RING_BUF_LOG2);

  dma_channel_configure((uint)spiDmaCh, &cfg, spiRingBuf, &SPI_PIO->rxf[SPI_SM],
                        0xFFFFFFFFu,
                        /*trigger=*/true);
}

// =============================================================================
// I2C slave init
// =============================================================================
static void i2cSlaveIrqHandler() {
  i2c_hw_t *hw = i2c_get_hw(i2c0);
  uint32_t intr = hw->intr_stat;

  if (intr & I2C_IC_INTR_STAT_R_RX_FULL_BITS) {
    uint32_t raw = hw->data_cmd;
    bool firstByte = (raw & I2C_IC_DATA_CMD_FIRST_DATA_BYTE_BITS) != 0u;
    uint8_t byte = static_cast<uint8_t>(raw & 0xFFu);

    if (firstByte) {
      i2cRxState = I2cRxState::WAIT_CTRL;
    }

    if (i2cRxState == I2cRxState::WAIT_CTRL) {
      // Decode SSD1306 control byte: bit6=D/C#, bit7=Co (Co=1 not supported)
      bool dc = (byte >> 6u) & 1u;
      i2cRxState = dc ? I2cRxState::STREAM_DATA : I2cRxState::STREAM_CMD;
    } else {
      uint32_t word = (i2cRxState == I2cRxState::STREAM_DATA)
                          ? (0x100u | byte)
                          : static_cast<uint32_t>(byte);
      i2cRingBuf[i2cWriteIdx] = word;
      i2cWriteIdx = (i2cWriteIdx + 1u) & (I2C_RING_BUF_WORDS - 1u);
    }
  }

  if (intr & I2C_IC_INTR_STAT_R_STOP_DET_BITS) {
    i2cRxState = I2cRxState::WAIT_CTRL;
    (void)hw->clr_stop_det;
  }

  if (intr & I2C_IC_INTR_STAT_R_RD_REQ_BITS) {
    hw->data_cmd = 0xFFu;
    (void)hw->clr_rd_req;
  }

  if (intr & I2C_IC_INTR_STAT_R_TX_ABRT_BITS) {
    (void)hw->clr_tx_abrt;
  }
}

static void i2cSlaveInit() {
  i2c_init(i2c0, 400u * 1000u);
  gpio_set_function(PIN_I2C_SDA, GPIO_FUNC_I2C);
  gpio_set_function(PIN_I2C_SCL, GPIO_FUNC_I2C);
  gpio_pull_up(PIN_I2C_SDA);
  gpio_pull_up(PIN_I2C_SCL);

  i2c_set_slave_mode(i2c0, true, I2C_SLAVE_ADDR);

  i2c_get_hw(i2c0)->intr_mask =
      I2C_IC_INTR_MASK_M_RX_FULL_BITS | I2C_IC_INTR_MASK_M_STOP_DET_BITS |
      I2C_IC_INTR_MASK_M_RD_REQ_BITS | I2C_IC_INTR_MASK_M_TX_ABRT_BITS;

  irq_set_exclusive_handler(I2C0_IRQ, i2cSlaveIrqHandler);
  irq_set_enabled(I2C0_IRQ, true);
}

// =============================================================================
// Interface switching (teardown current, setup new)
// =============================================================================
static void switchInterface(InterfaceType newIface) {
  if (gIfaceActive) {
    if (gCurrentIface == InterfaceType::I2C) {
      irq_set_enabled(I2C0_IRQ, false);
      i2c_deinit(i2c0);
      i2cReadIdx = 0;
      i2cWriteIdx = 0;
      i2cRxState = I2cRxState::WAIT_CTRL;
    } else {
      if (spiDmaCh >= 0) {
        dma_channel_abort((uint)spiDmaCh);
        dma_channel_unclaim((uint)spiDmaCh);
        spiDmaCh = -1;
      }
      pio_sm_set_enabled(SPI_PIO, SPI_SM, false);
      pio_sm_clear_fifos(SPI_PIO, SPI_SM);
      if (gCurrentIface == InterfaceType::SPI_4LINE ||
          gCurrentIface == InterfaceType::SPI_3LINE) {
        gpio_set_irq_enabled(PIN_SPI_CS, GPIO_IRQ_EDGE_RISE, false);
      }
      if (gCurrentPioProgram) {
        pio_remove_program(SPI_PIO, gCurrentPioProgram, gSpiProgOffset);
        gCurrentPioProgram = nullptr;
      }
      spiReadIdx = 0;
    }
    if (gInst) {
      gInst->inputReset(true);
      gInst->inputReset(false);
    }
  }
  gIfaceActive = true;

  switch (newIface) {
    case InterfaceType::I2C: i2cSlaveInit(); break;
    case InterfaceType::SPI_4LINE:
      gpio_set_irq_enabled(PIN_SPI_CS, GPIO_IRQ_EDGE_RISE, true);
      gCurrentPioProgram = &spi_4line_mode0_program;
      gSpiProgOffset = pio_add_program(SPI_PIO, gCurrentPioProgram);
      spiSlaveInit(gSpiProgOffset);
      spiDmaInit();
      break;
    case InterfaceType::SPI_3LINE:
      gpio_set_irq_enabled(PIN_SPI_CS, GPIO_IRQ_EDGE_RISE, true);
      gCurrentPioProgram = &spi_3line_mode0_program;
      gSpiProgOffset = pio_add_program(SPI_PIO, gCurrentPioProgram);
      spi3lineSlaveInit(gSpiProgOffset);
      spiDmaInit();
      break;
    case InterfaceType::PARALLEL:
      gCurrentPioProgram = &parallel_8bit_program;
      gSpiProgOffset = pio_add_program(SPI_PIO, gCurrentPioProgram);
      parSlaveInit(gSpiProgOffset);
      spiDmaInit();
      break;
  }
  gCurrentIface = newIface;
}

// =============================================================================
// Drain the SPI/Parallel ring buffer and feed bytes to LcdTap
// =============================================================================
static void processSpiRingBuf() {
  uint32_t writeAddr = dma_channel_hw_addr((uint)spiDmaCh)->write_addr;
  uint32_t writeIdx =
      (writeAddr - reinterpret_cast<uint32_t>(spiRingBuf)) / sizeof(uint32_t);
  writeIdx &= (SPI_RING_BUF_WORDS - 1u);

  if (!gInst) {
    spiReadIdx = writeIdx;
    return;
  }

  uint32_t dataStart = spiReadIdx;
  while (spiReadIdx != writeIdx) {
    uint32_t lastReadIdx = spiReadIdx;
    uint32_t word = spiRingBuf[spiReadIdx];
    spiReadIdx = (spiReadIdx + 1u) & (SPI_RING_BUF_WORDS - 1u);

    if (word & 0x100u) {
      if (spiReadIdx == 0) {
        gInst->inputData((uint8_t *)&spiRingBuf[dataStart],
                         (SPI_RING_BUF_WORDS - dataStart), sizeof(uint32_t));
        dataStart = 0;
      }
    } else {
      uint32_t dataLen = lastReadIdx - dataStart;
      if (dataLen != 0) {
        gInst->inputData((uint8_t *)&spiRingBuf[dataStart], dataLen,
                         sizeof(uint32_t));
      }
      gInst->inputCommand(static_cast<uint8_t>(word));
      dataStart = spiReadIdx;
    }
  }

  uint32_t dataLen = spiReadIdx - dataStart;
  if (dataLen != 0) {
    gInst->inputData((uint8_t *)&spiRingBuf[dataStart], dataLen,
                     sizeof(uint32_t));
  }
}

// =============================================================================
// Drain the I2C ring buffer and feed bytes to LcdTap
// =============================================================================
static void processI2cRingBuf() {
  uint32_t writeIdx = i2cWriteIdx;  // snapshot of volatile

  if (!gInst) {
    i2cReadIdx = writeIdx;
    return;
  }

  uint32_t dataStart = i2cReadIdx;
  while (i2cReadIdx != writeIdx) {
    uint32_t lastReadIdx = i2cReadIdx;
    uint32_t word = i2cRingBuf[i2cReadIdx];
    i2cReadIdx = (i2cReadIdx + 1u) & (I2C_RING_BUF_WORDS - 1u);

    if (word & 0x100u) {
      if (i2cReadIdx == 0) {
        gInst->inputData((uint8_t *)&i2cRingBuf[dataStart],
                         (I2C_RING_BUF_WORDS - dataStart), sizeof(uint32_t));
        dataStart = 0;
      }
    } else {
      uint32_t dataLen = lastReadIdx - dataStart;
      if (dataLen != 0) {
        gInst->inputData((uint8_t *)&i2cRingBuf[dataStart], dataLen,
                         sizeof(uint32_t));
      }
      gInst->inputCommand(static_cast<uint8_t>(word));
      dataStart = i2cReadIdx;
    }
  }

  uint32_t dataLen = i2cReadIdx - dataStart;
  if (dataLen != 0) {
    gInst->inputData((uint8_t *)&i2cRingBuf[dataStart], dataLen,
                     sizeof(uint32_t));
  }
}

// =============================================================================
// Dispatch to the active ring buffer processor
// =============================================================================
static void processInputBuf() {
  if (gCurrentIface == InterfaceType::I2C)
    processI2cRingBuf();
  else
    processSpiRingBuf();
}

// =============================================================================
// Read key inputs; returns OSD_KEY_XXX bitmask.
// All keys are active-low with internal pull-ups.
// =============================================================================
static uint8_t readKeys() {
  uint8_t keys = 0;
  if (!gpio_get(PIN_KEY_UP)) keys |= lcdtap::OSD_KEY_UP;
  if (!gpio_get(PIN_KEY_DOWN)) keys |= lcdtap::OSD_KEY_DOWN;
  if (!gpio_get(PIN_KEY_LEFT)) keys |= lcdtap::OSD_KEY_LEFT;
  if (!gpio_get(PIN_KEY_RIGHT)) keys |= lcdtap::OSD_KEY_RIGHT;
  if (!gpio_get(PIN_KEY_ENTER)) keys |= lcdtap::OSD_KEY_ENTER;
  return keys;
}

// =============================================================================
// main
// =============================================================================
int main() {
  // -------------------------------------------------------------------------
  // 1. Read boot-time configuration GPIOs and initialize key inputs
  // -------------------------------------------------------------------------

  for (uint pin :
       {PIN_KEY_UP, PIN_KEY_DOWN, PIN_KEY_LEFT, PIN_KEY_RIGHT, PIN_KEY_ENTER}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
  }

  gpio_init(PIN_CFG_DVI_RES);
  gpio_set_dir(PIN_CFG_DVI_RES, GPIO_IN);
  gpio_pull_down(PIN_CFG_DVI_RES);
  sleep_ms(1);

  const bool dvi720p = gpio_get(PIN_CFG_DVI_RES);

  const uint16_t lcdW = LCDTAP_LCD_SIZE_W;
  const uint16_t lcdH = LCDTAP_LCD_SIZE_H;

  const struct dvi_timing *timing =
      dvi720p ? &dvi_timing_1280x720p_reduced_30hz : &dvi_timing_640x480p_60hz;

  // -------------------------------------------------------------------------
  // 2. Voltage regulator and system clock
  // -------------------------------------------------------------------------
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  sleep_ms(10);
  set_sys_clock_khz(timing->bit_clk_khz, /*required=*/true);

  // -------------------------------------------------------------------------
  // 3. LED
  // -------------------------------------------------------------------------
  gpio_init(PIN_LED);
  gpio_set_dir(PIN_LED, GPIO_OUT);
  gpio_put(PIN_LED, 0);

  // -------------------------------------------------------------------------
  // 4. RESX input (pull-up; active low from SPI/Parallel master)
  // -------------------------------------------------------------------------
  gpio_init(PIN_RESX);
  gpio_set_dir(PIN_RESX, GPIO_IN);
  gpio_pull_up(PIN_RESX);

  // -------------------------------------------------------------------------
  // 5. DVI init (claims DMA channels and PIO0 state machines)
  // -------------------------------------------------------------------------
  dvi0.timing = timing;
  dvi0.ser_cfg = pico_sock_cfg;
  dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

  const uint32_t dviW = timing->h_active_pixels / 2;
  const uint32_t dviH = timing->v_active_lines / DVI_VERTICAL_REPEAT;

  for (int i = 0; i < N_SCANLINE_BUFS; ++i) {
    uint16_t *p = scanlineBufs[i];
    queue_add_blocking_u32(&dvi0.q_colour_free, &p);
  }

  // -------------------------------------------------------------------------
  // 6. LcdTap init
  // -------------------------------------------------------------------------
  lcdtap::LcdTapConfig cfg;
  lcdtap::getDefaultConfig(lcdtap::ControllerType::ST7789, &cfg);
  cfg.lcdWidth = lcdW;
  cfg.lcdHeight = lcdH;
  cfg.scaleMode = lcdtap::ScaleMode::FIT;

  cfg.dviWidth = static_cast<uint16_t>(dviW);
  cfg.dviHeight = static_cast<uint16_t>(dviH);

  // Restore settings saved to flash; DVI dimensions and interface type are
  // restored here; interface is applied later via switchInterface().
  {
    ConfigFile saved;
    if (loadConfig(&saved)) {
      saved.libConfig.dviWidth = cfg.dviWidth;
      saved.libConfig.dviHeight = cfg.dviHeight;
      cfg = saved.libConfig;
      gCurrentIface = saved.interfaceType;
    }
  }

  lcdtap::HostInterface host;
  host.alloc = poolAlloc;
  host.free = poolFree;
  host.log = nullptr;
  host.userData = nullptr;

  lcdtap::LcdTap inst(cfg, host);
  if (inst.getStatus() != lcdtap::Status::OK) panic("LcdTap init failed");

  {
    static const uint16_t kBars[8] = {
        0x0000u, 0x001Fu, 0x07E0u, 0x07FFu, 0xF800u, 0xF81Fu, 0xFFE0u, 0xFFFFu,
    };
    uint16_t *fb = inst.getFramebuf();
    for (uint16_t y = 0; y < lcdH; ++y) {
      for (uint16_t x = 0; x < lcdW; ++x) {
        fb[(uint32_t)y * lcdW + x] = kBars[(uint32_t)x * 8u / lcdW];
      }
    }
    inst.setDisplayOn(true);
  }

  gInst = &inst;

  // -------------------------------------------------------------------------
  // 6b. OSD init
  // -------------------------------------------------------------------------
  lcdtap::OsdConfig osdCfg;
  lcdtap::getDefaultOsdConfig(&osdCfg);
  osdCfg.onMenuOpen = onOsdMenuOpen;
  osdCfg.userData = nullptr;
  gOsd.init(osdCfg);

  // -------------------------------------------------------------------------
  // 7. RESX interrupt
  // -------------------------------------------------------------------------
  gpio_set_irq_enabled_with_callback(PIN_RESX,
                                     GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                     /*enabled=*/true, &gpioIrqHandler);

  // -------------------------------------------------------------------------
  // 8. Input slave PIO + DMA (after dvi_init so DVI claims its channels first)
  // -------------------------------------------------------------------------
  switchInterface(gCurrentIface);

  // -------------------------------------------------------------------------
  // 9. Launch Core 1 for DVI TMDS output
  // -------------------------------------------------------------------------
  multicore_launch_core1(core1Main);

  // -------------------------------------------------------------------------
  // 10. Main loop (Core 0)
  //     processInputBuf() is called at three points per iteration:
  //       (a) inside the q_colour_free spin — drains while waiting
  //       (b) just before getScanline — drains during scanline preparation
  //       (c) after queue_add_blocking_u32 — drains after handing off
  // -------------------------------------------------------------------------
  uint32_t scanY = 0;
  uint32_t frame = 0;
  bool led = false;

  while (true) {
    uint16_t *buf;
    while (!queue_try_remove_u32(&dvi0.q_colour_free, &buf)) {
      processInputBuf();  // (a) drain while waiting for free buffer
    }

    gInst->fillScanline(static_cast<uint16_t>(scanY), buf);
    gOsd.fillScanline(static_cast<uint16_t>(scanY), buf);

    queue_add_blocking_u32(&dvi0.q_colour_valid, &buf);

    if (++scanY >= dviH) {
      scanY = 0;
      uint64_t nowMs =
          static_cast<uint64_t>(to_ms_since_boot(get_absolute_time()));
      uint8_t action = gOsd.update(nowMs, *gInst, readKeys());
      if (action == lcdtap::OSD_ACTION_APPLY) {
        const lcdtap::OsdMenuItem *ifaceItem = nullptr;
        gOsd.getItemById(OSD_USER_ITEM_ID_INTERFACE, &ifaceItem);
        InterfaceType newIface =
            ifaceItem ? static_cast<InterfaceType>(ifaceItem->value)
                      : gCurrentIface;

        ConfigFile toSave;
        toSave.libConfig = gInst->getConfig();
        toSave.interfaceType = newIface;
        saveConfig(toSave);

        if (newIface != gCurrentIface) {
          switchInterface(newIface);
        }
      }
      if (++frame % LED_TOGGLE_FRAMES == 0u) {
        led = !led;
        gpio_put(PIN_LED, led ? 1 : 0);
      }
    }

    processInputBuf();  // (b)+(c) drain after handing off scanline
  }
}
