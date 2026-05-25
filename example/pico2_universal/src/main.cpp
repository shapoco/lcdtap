#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "common_dvi_pin_configs.h"  // pico_sock_cfg
#include "dvi.h"
#include "dvi_timing.h"

#include "lcdtap/lcdtap.hpp"
#include "lcdtap/osd.hpp"
#include "lcdtap/pico2/dvi_out.hpp"
#include "lcdtap/pico2/i2c_slave.hpp"
#include "lcdtap/pico2/spi_slave.hpp"

#include "config.h"
#include "flash_config.hpp"
#include "parallel_8bit.pio.h"
#include "spi_3line_mode0.pio.h"
#include "uart_if.hpp"

static_assert(PIN_SPI_CS == SPI_CS_PIN,
              "PIN_SPI_CS mismatch with spi_4line_mode0.pio");
static_assert(PIN_SPI_SCLK == SPI_SCLK_PIN,
              "PIN_SPI_SCLK mismatch with spi_4line_mode0.pio");
static_assert(PIN_SPI_MOSI + 1u == PIN_SPI_DC,
              "DC must be MOSI+1 for 'in pins, 2'");
static_assert(PIN_SPI_CS == PAR_CS_PIN,
              "PIN_SPI_CS mismatch with parallel_8bit.pio PAR_CS_PIN");
static_assert(PIN_PAR_CS == PAR_CS_PIN,
              "PIN_PAR_CS mismatch with parallel_8bit.pio PAR_CS_PIN");
static_assert(PIN_PAR_WR == PAR_WR_PIN,
              "PIN_PAR_WR mismatch with parallel_8bit.pio PAR_WR_PIN");
static_assert(PIN_PAR_DC == PIN_PAR_DATA_BASE + 8u,
              "PIN_PAR_DC must be PAR_DATA_BASE+8 for 'in pins, 9'");

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
// DMA alignment constraint: must be aligned to its own byte size.
// =============================================================================
static uint32_t
    __attribute__((aligned(SPI_RING_BUF_BYTES))) spiRingBuf[SPI_RING_BUF_WORDS];

// =============================================================================
// I2C ring buffer  (same word format as SPI ring buffer)
// =============================================================================
static uint32_t i2cRingBuf[I2C_RING_BUF_WORDS];

// =============================================================================
// Interface selection
// =============================================================================
static InterfaceType gCurrentIface = InterfaceType::SPI_4LINE;
static bool gIfaceActive = false;

// =============================================================================
// DVI
// =============================================================================
static struct dvi_inst dvi0;

static uint16_t
    __attribute__((aligned(4))) scanlineBufs[N_SCANLINE_BUFS][DVI_MAX_W];

// =============================================================================
// LcdTap instance
// =============================================================================
static lcdtap::LcdTap *gInst = nullptr;

// OSD instance for runtime configuration
static lcdtap::Osd gOsd;

// =============================================================================
// Module state
// =============================================================================
static lcdtap::pico2::SpiSlaveState gSpi;
static lcdtap::pico2::I2cSlaveState gI2c;
static lcdtap::pico2::DviOutState gDvi;

// =============================================================================
// OSD user items
// =============================================================================
static constexpr uint16_t OSD_USER_ITEM_ID_PRESET_ST7789 =
    lcdtap::OSD_USER_ITEM_ID_BASE;
static constexpr uint16_t OSD_USER_ITEM_ID_PRESET_SSD1306 =
    lcdtap::OSD_USER_ITEM_ID_BASE + 1u;
static constexpr uint16_t OSD_USER_ITEM_ID_PRESET_SSD1331 =
    lcdtap::OSD_USER_ITEM_ID_BASE + 2u;
static constexpr uint16_t OSD_USER_ITEM_ID_INTERFACE =
    lcdtap::OSD_USER_ITEM_ID_BASE + 3u;

static const char *kInterfaceNames[] = {"I2C", "4Line SPI", "3Line SPI",
                                        "Parallel"};

static void onOsdMenuOpen(lcdtap::Osd *osd, void * /*userData*/) {
  lcdtap::OsdMenuItem p = {};
  p.type = lcdtap::OsdMenuType::ACTION;
  p.unit = "";
  p.options = nullptr;
  p.value = 0;

  p.id = OSD_USER_ITEM_ID_PRESET_ST7789;
  p.name = "Preset:ST7789";
  osd->insertItem(0, p);

  p.id = OSD_USER_ITEM_ID_PRESET_SSD1306;
  p.name = "Preset:SSD1306";
  osd->insertItem(1, p);

  p.id = OSD_USER_ITEM_ID_PRESET_SSD1331;
  p.name = "Preset:SSD1331";
  osd->insertItem(2, p);

  lcdtap::OsdMenuItem item = {};
  item.id = OSD_USER_ITEM_ID_INTERFACE;
  item.type = lcdtap::OsdMenuType::ENUM;
  item.name = "Interface";
  item.unit = "";
  item.options = kInterfaceNames;
  item.min = 0;
  item.max = 3;
  item.step = 1;
  item.value = static_cast<int16_t>(gCurrentIface);
  osd->insertItem(3, item);

  osd->setSelectedIndex(0);
}

static bool onOsdActionActivated(lcdtap::Osd *osd,
                                 const lcdtap::OsdMenuItem *item,
                                 lcdtap::LcdTap &lcdtap, void * /*userData*/) {
  lcdtap::ControllerType ct;
  InterfaceType iface;
  if (item->id == OSD_USER_ITEM_ID_PRESET_ST7789) {
    ct = lcdtap::ControllerType::ST7789;
    iface = InterfaceType::SPI_4LINE;
  } else if (item->id == OSD_USER_ITEM_ID_PRESET_SSD1306) {
    ct = lcdtap::ControllerType::SSD1306;
    iface = InterfaceType::I2C;
  } else if (item->id == OSD_USER_ITEM_ID_PRESET_SSD1331) {
    ct = lcdtap::ControllerType::SSD1331;
    iface = InterfaceType::SPI_4LINE;
  } else {
    return false;  // default handling
  }
  lcdtap::LcdTapConfig preset;
  lcdtap::getDefaultConfig(ct, &preset);
  preset.dviWidth = lcdtap.getConfig().dviWidth;
  preset.dviHeight = lcdtap.getConfig().dviHeight;
  osd->loadConfig(preset);
  osd->setItemValue(OSD_USER_ITEM_ID_INTERFACE, static_cast<int16_t>(iface));
  return true;  // keep OSD open
}

// =============================================================================
// 3-Line SPI slave init  (PIO1 SM0)
// =============================================================================
static void spi3lineSlaveInit(uint progOffset) {
  for (uint pin : {PIN_SPI_SCLK, PIN_SPI_MOSI}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
  }
  gpio_init(PIN_SPI_CS);
  gpio_set_dir(PIN_SPI_CS, GPIO_IN);
  gpio_pull_up(PIN_SPI_CS);

  pio_sm_config c = spi_3line_mode0_program_get_default_config(progOffset);
  sm_config_set_in_pins(&c, PIN_SPI_MOSI);  // IN_BASE=GPIO3 (MOSI only)
  sm_config_set_in_shift(&c, /*shift_direction=*/false, /*autopush=*/false,
                         /*push_threshold=*/32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  sm_config_set_jmp_pin(&c, PIN_SPI_CS);

  pio_sm_init(SPI_PIO, SPI_SM, progOffset, &c);
  pio_sm_set_enabled(SPI_PIO, SPI_SM, true);
}

// =============================================================================
// Parallel slave init  (PIO1 SM0)
// =============================================================================
static void parSlaveInit(uint progOffset) {
  gpio_init(PIN_PAR_CS);
  gpio_set_dir(PIN_PAR_CS, GPIO_IN);
  gpio_pull_up(PIN_PAR_CS);
  gpio_init(PIN_PAR_WR);
  gpio_set_dir(PIN_PAR_WR, GPIO_IN);
  for (uint pin = PIN_PAR_DATA_BASE; pin < PIN_PAR_DATA_BASE + 8u; ++pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
  }
  gpio_init(PIN_PAR_DC);
  gpio_set_dir(PIN_PAR_DC, GPIO_IN);

  pio_sm_config c = parallel_8bit_program_get_default_config(progOffset);
  sm_config_set_in_pins(&c, PIN_PAR_DATA_BASE);
  sm_config_set_in_shift(&c, /*shift_direction=*/false, /*autopush=*/false,
                         /*push_threshold=*/32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

  pio_sm_init(SPI_PIO, SPI_SM, progOffset, &c);
  pio_sm_set_enabled(SPI_PIO, SPI_SM, true);
}

// =============================================================================
// GPIO interrupt handler  (RST pin; CS pin for SPI/Parallel modes)
// =============================================================================
static void __not_in_flash_func(gpioIrqHandler)(uint gpio, uint32_t events) {
  if (gpio == PIN_SPI_CS) {
    if (events & GPIO_IRQ_EDGE_RISE) {
      lcdtap::pico2::spiSlaveResetSm(&gSpi);
    }
  } else if (gpio == PIN_RST && gInst) {
    if (events & GPIO_IRQ_EDGE_FALL) {
      gInst->inputReset(true);
      lcdtap::pico2::spiSlaveResetSm(&gSpi);
    }
    gInst->inputReset(!gpio_get(PIN_RST));
  }
}

// =============================================================================
// Interface switching (teardown current, setup new)
// Also used as a callback from uartIfInit().
// =============================================================================
static void switchInterface(InterfaceType newIface) {
  if (gIfaceActive) {
    if (gCurrentIface == InterfaceType::I2C) {
      lcdtap::pico2::i2cSlaveDeinit(&gI2c);
    } else {
      lcdtap::pico2::spiSlaveDeinit(&gSpi);
    }
    if (gInst) {
      gInst->inputReset(true);
      gInst->inputReset(false);
    }
  }
  gIfaceActive = true;

  switch (newIface) {
    case InterfaceType::I2C: {
      lcdtap::pico2::I2cSlaveConfig i2cCfg = {i2c0, PIN_I2C_SDA, PIN_I2C_SCL,
                                              I2C_SLAVE_ADDR};
      lcdtap::pico2::i2cSlaveInit(&gI2c, i2cCfg, i2cRingBuf,
                                  I2C_RING_BUF_WORDS);
      gI2c.inst = gInst;
      break;
    }
    case InterfaceType::SPI_4LINE: {
      lcdtap::pico2::SpiSlaveConfig spiCfg = {
          SPI_PIO,      SPI_SM,     PIN_SPI_CS,       PIN_SPI_SCLK,
          PIN_SPI_MOSI, PIN_SPI_DC, SPI_RING_BUF_LOG2};
      lcdtap::pico2::spiSlaveInit(&gSpi, spiCfg, spiRingBuf,
                                  SPI_RING_BUF_WORDS);
      gSpi.inst = gInst;
      lcdtap::pico2::spiSlaveRegisterIrq(&gSpi);
      break;
    }
    case InterfaceType::SPI_3LINE: {
      gSpi.progOffset = pio_add_program(SPI_PIO, &spi_3line_mode0_program);
      gSpi.pioProgram = &spi_3line_mode0_program;
      spi3lineSlaveInit(gSpi.progOffset);
      lcdtap::pico2::spiSlaveInitDma(&gSpi);
      gpio_set_irq_enabled(PIN_SPI_CS, GPIO_IRQ_EDGE_RISE, true);
      break;
    }
    case InterfaceType::PARALLEL: {
      gSpi.progOffset = pio_add_program(SPI_PIO, &parallel_8bit_program);
      gSpi.pioProgram = &parallel_8bit_program;
      parSlaveInit(gSpi.progOffset);
      lcdtap::pico2::spiSlaveInitDma(&gSpi);
      gpio_set_irq_enabled(PIN_SPI_CS, GPIO_IRQ_EDGE_RISE, true);
      break;
    }
  }
  gCurrentIface = newIface;
}

// =============================================================================
// Dispatch to the active ring buffer processor
// =============================================================================
static void processInputBuf() {
  if (gCurrentIface == InterfaceType::I2C)
    lcdtap::pico2::i2cSlaveProcess(&gI2c);
  else
    lcdtap::pico2::spiSlaveProcess(&gSpi);
}

// =============================================================================
// OSD overlay for Core 1 (called after LcdTap::fillScanline)
// =============================================================================
static void universalFillScanline(uint16_t scanY, uint16_t *buf,
                                  void * /*userData*/) {
  gOsd.fillScanline(scanY, buf);
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

  gpio_init(PIN_CFG_OUT_720P);
  gpio_set_dir(PIN_CFG_OUT_720P, GPIO_IN);
  gpio_pull_up(PIN_CFG_OUT_720P);
  sleep_ms(1);

  const bool dvi720p = !gpio_get(PIN_CFG_OUT_720P);  // LOW=720p (active-low)

  const uint16_t lcdW = LCDTAP_LCD_SIZE_W;
  const uint16_t lcdH = LCDTAP_LCD_SIZE_H;

  const struct dvi_timing *timing =
      dvi720p ? &dvi_timing_1280x720p_reduced_30hz : &dvi_timing_640x480p_60hz;

  // -------------------------------------------------------------------------
  // 1b. Read flash config — check for USB mass storage boot mode
  // -------------------------------------------------------------------------
  ConfigFile savedCfg = {};
  bool hasSavedCfg = loadConfig(&savedCfg);

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
  // 4. RST input (pull-up; active low from SPI/Parallel master)
  // -------------------------------------------------------------------------
  gpio_init(PIN_RST);
  gpio_set_dir(PIN_RST, GPIO_IN);
  gpio_pull_up(PIN_RST);

  // -------------------------------------------------------------------------
  // 5. DVI init (claims DMA channels and PIO0 state machines)
  // -------------------------------------------------------------------------
  dvi0.timing = timing;
  dvi0.ser_cfg = pico_sock_cfg;
  dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

  const uint32_t dviW = timing->h_active_pixels / 2;
  const uint32_t dviH = timing->v_active_lines / DVI_VERTICAL_REPEAT;

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

  if (hasSavedCfg) {
    savedCfg.libConfig.dviWidth = cfg.dviWidth;
    savedCfg.libConfig.dviHeight = cfg.dviHeight;
    cfg = savedCfg.libConfig;
    gCurrentIface = savedCfg.interfaceType;
  }

  lcdtap::HostInterface host;
  host.alloc = poolAlloc;
  host.free = poolFree;
  host.log = nullptr;
  host.userData = nullptr;

  lcdtap::LcdTap inst(cfg, host);
  if (inst.getStatus() != lcdtap::Status::OK) panic("LcdTap init failed");

  gInst = &inst;

  // -------------------------------------------------------------------------
  // 6b. OSD init
  // -------------------------------------------------------------------------
  lcdtap::OsdConfig osdCfg;
  lcdtap::getDefaultOsdConfig(&osdCfg);
  osdCfg.onMenuOpen = onOsdMenuOpen;
  osdCfg.onActionActivated = onOsdActionActivated;
  osdCfg.userData = nullptr;
  gOsd.init(osdCfg);

  // -------------------------------------------------------------------------
  // 7. RST interrupt
  // -------------------------------------------------------------------------
  gpio_set_irq_enabled_with_callback(PIN_RST,
                                     GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                     /*enabled=*/true, &gpioIrqHandler);
  irq_set_priority(IO_IRQ_BANK0, 0x00);

  // -------------------------------------------------------------------------
  // 8. Pre-initialize gSpi common fields so switchInterface can use them for
  //    any SPI variant (4-line, 3-line, parallel) without re-specifying them.
  // -------------------------------------------------------------------------
  gSpi.cfg = {SPI_PIO,      SPI_SM,     PIN_SPI_CS,       PIN_SPI_SCLK,
              PIN_SPI_MOSI, PIN_SPI_DC, SPI_RING_BUF_LOG2};
  gSpi.ringBuf = spiRingBuf;
  gSpi.ringWords = SPI_RING_BUF_WORDS;
  gSpi.inst = &inst;
  gSpi.dmaCh = -1;
  gSpi.pioProgram = nullptr;

  // -------------------------------------------------------------------------
  // 9. Input slave PIO + DMA (after dvi_init so DVI claims its channels first)
  // -------------------------------------------------------------------------
  switchInterface(gCurrentIface);

  // -------------------------------------------------------------------------
  // 10. Launch Core 1 (fillScanline + OSD overlay + TMDS encode + serialise)
  // -------------------------------------------------------------------------
  lcdtap::pico2::DviOutConfig dviCfg = {PIN_LED, LED_TOGGLE_FRAMES};
  lcdtap::pico2::dviOutPrepare(&gDvi, &dvi0, scanlineBufs[0],
                               sizeof(scanlineBufs[0]), N_SCANLINE_BUFS, &inst,
                               universalFillScanline, nullptr, dviCfg);
  gDvi.dviH = dviH;
  lcdtap::pico2::dviOutLaunchCore1(&gDvi);

  // -------------------------------------------------------------------------
  // 11. USB CDC serial interface
  // -------------------------------------------------------------------------
  uartIfInit(&inst, &gCurrentIface, switchInterface, saveConfig);

  // -------------------------------------------------------------------------
  // 12. Main loop (Core 0)
  //     Core 1 handles fillScanline + TMDS encode; Core 0 is free to drain
  //     the SPI/I2C ring buffers continuously without timer preemption.
  //     dviOutConsumeNewFrame returns true once per frame boundary.
  // -------------------------------------------------------------------------
  while (true) {
    processInputBuf();
    uartIfProcess();
    if (lcdtap::pico2::dviOutConsumeNewFrame(&gDvi)) {
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
    }
  }
}
