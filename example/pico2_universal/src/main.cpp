#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "lcdtap/lcdtap.hpp"
#include "lcdtap/osd.hpp"
#include "lcdtap/pico2/composite_out.hpp"
#include "lcdtap/pico2/hstx_out.hpp"
#include "lcdtap/pico2/i2c_slave.hpp"
#include "lcdtap/pico2/spi_slave.hpp"

#include "composite_dac_kind.hpp"
#include "config.h"
#include "flash_config.hpp"
#include "output_interface.hpp"
#include "parallel_8bit.pio.h"
#include "spi_3line_mode0.pio.h"
#include "uart_intf.hpp"
#include "video_backend.hpp"

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
static lcdtap::BusType gCurrentIface = lcdtap::BusType::SPI_4LINE;
static bool gIfaceActive = false;

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
static lcdtap::pico2::HstxOutState gHstx;
static lcdtap::pico2::CompositeOutState gCvbs;

// =============================================================================
// Video backend selection
// Init keeps its real signatures (see video_backend.hpp); only the
// steady-state operations are dispatched indirectly.
// =============================================================================
static const VideoBackend BACKEND_HSTX = {
    [](void *s) {
      lcdtap::pico2::hstxOutLaunchCore1((lcdtap::pico2::HstxOutState *)s);
    },
    [](void *s) {
      return lcdtap::pico2::hstxOutConsumeNewFrame(
          (lcdtap::pico2::HstxOutState *)s);
    },
    [](void *s) {
      lcdtap::pico2::hstxOutFlashAcquire((lcdtap::pico2::HstxOutState *)s);
    },
    [](void *s) {
      lcdtap::pico2::hstxOutFlashRelease((lcdtap::pico2::HstxOutState *)s);
    },
};

static const VideoBackend BACKEND_COMPOSITE = {
    [](void *s) {
      lcdtap::pico2::compositeOutLaunchCore1(
          (lcdtap::pico2::CompositeOutState *)s);
    },
    [](void *s) {
      return lcdtap::pico2::compositeOutConsumeNewFrame(
          (lcdtap::pico2::CompositeOutState *)s);
    },
    [](void *s) {
      lcdtap::pico2::compositeOutFlashAcquire(
          (lcdtap::pico2::CompositeOutState *)s);
    },
    [](void *s) {
      lcdtap::pico2::compositeOutFlashRelease(
          (lcdtap::pico2::CompositeOutState *)s);
    },
};

static const VideoBackend *gVideo = &BACKEND_HSTX;
static void *gVideoState = &gHstx;

// Currently active output interface and composite DAC (mirror ConfigFile).
static OutputInterface gOutputIf = OutputInterface::DVI_D;
static CompositeDacKind gCvbsDac = CompositeDacKind::PWM;

// =============================================================================
// OSD user items
// =============================================================================

// User item IDs must be >= OSD_USER_ITEM_ID_BASE so they never collide with
// the library's system items.
static constexpr uint16_t OSD_ITEM_ID_OUTPUT_IF =
    lcdtap::OSD_USER_ITEM_ID_BASE + 0u;
static constexpr uint16_t OSD_ITEM_ID_CVBS_DAC =
    lcdtap::OSD_USER_ITEM_ID_BASE + 1u;

static void onOsdMenuOpen(lcdtap::Osd *osd, void * /*userData*/) {
  // Called after initMenuItems() has populated every system item, so Apply
  // already exists and can be used as an anchor.
  const int16_t busKeyId =
      lcdtap::OSD_ITEM_ID_SYS_BASE +
      static_cast<int16_t>(lcdtap::ConfigId::BUS_INTERFACE);

  lcdtap::OsdMenuItem out = {};
  out.id = OSD_ITEM_ID_OUTPUT_IF;
  out.isAction = false;
  out.config.type = lcdtap::ValueType::ENUM;
  out.config.name = "Output Interface";
  out.config.unit = "";
  out.config.options = OUTPUT_INTERFACE_NAMES;
  out.config.min = 0;
  out.config.max = OUTPUT_INTERFACE_COUNT - 1;
  out.config.step = 1;
  out.config.value = static_cast<int16_t>(gOutputIf);
  // enableKeyId is auto-offset from ConfigId to OSD id only for system items
  // (osd.cpp initMenuItems), so a user item must supply a resolved id.
  // Composite needs pins the parallel bus already owns.
  out.config.enableKeyId = busKeyId;
  out.config.enableKeyValueMin = static_cast<int16_t>(lcdtap::BusType::I2C);
  out.config.enableKeyValueMax =
      static_cast<int16_t>(lcdtap::BusType::SPI_3LINE);
  // insertItem() does not run updateItemEnables(), so seed isEnabled here.
  out.isEnabled = (gCurrentIface != lcdtap::BusType::PARALLEL);

  lcdtap::OsdMenuItem dac = {};
  dac.id = OSD_ITEM_ID_CVBS_DAC;
  dac.isAction = false;
  dac.config.type = lcdtap::ValueType::ENUM;
  dac.config.name = "NTSC/PAL DAC Type";
  dac.config.unit = "";
  dac.config.options = COMPOSITE_DAC_KIND_NAMES;
  dac.config.min = 0;
  dac.config.max = COMPOSITE_DAC_KIND_COUNT - 1;
  dac.config.step = 1;
  dac.config.value = static_cast<int16_t>(gCvbsDac);
  // Gated on SPI rather than on the Output item: the library allows one
  // enable key per item, so it gates on the axis where a wrong value is a
  // hardware conflict (the R-2R ladder covers the I2C pins) rather than the
  // axis where it is merely inert (DVI-D). compositeDacSanitize() cleans up
  // the inert case at Apply.
  dac.config.enableKeyId = OSD_ITEM_ID_OUTPUT_IF;
  dac.config.enableKeyValueMin = static_cast<int16_t>(OutputInterface::NTSC);
  dac.config.enableKeyValueMax = static_cast<int16_t>(OutputInterface::PAL);
  dac.isEnabled = compositeDacAllowed(CompositeDacKind::R2R, gCurrentIface);

  // Insert before the anchor setting. The first insert shifts the anchor down
  // by one, so it has to be looked up again for the second. The UART parameter
  // list uses the same anchor, so both orders match.
  uint16_t insertId =
      lcdtap::OSD_ITEM_ID_SYS_BASE + static_cast<int16_t>(HOST_PARAM_ANCHOR);
  osd->insertItem(osd->getItemIndexById(insertId), out);
  osd->insertItem(osd->getItemIndexById(insertId), dac);
}

static bool onOsdActionActivated(lcdtap::Osd *osd,
                                 const lcdtap::OsdMenuItem *item,
                                 lcdtap::LcdTap &lcdtap, void * /*userData*/) {
  return false;
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
static void switchInterface(lcdtap::BusType newIface) {
  if (gIfaceActive) {
    if (gCurrentIface == lcdtap::BusType::I2C) {
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
    case lcdtap::BusType::I2C: {
      lcdtap::pico2::I2cSlaveConfig i2cCfg = {i2c0, PIN_I2C_SDA, PIN_I2C_SCL,
                                              I2C_SLAVE_ADDR};
      lcdtap::pico2::i2cSlaveInit(&gI2c, i2cCfg, i2cRingBuf,
                                  I2C_RING_BUF_WORDS);
      gI2c.inst = gInst;
      break;
    }
    case lcdtap::BusType::SPI_4LINE: {
      lcdtap::pico2::SpiSlaveConfig spiCfg = {
          SPI_PIO,      SPI_SM,     PIN_SPI_CS,       PIN_SPI_SCLK,
          PIN_SPI_MOSI, PIN_SPI_DC, SPI_RING_BUF_LOG2};
      lcdtap::pico2::spiSlaveInit(&gSpi, spiCfg, spiRingBuf,
                                  SPI_RING_BUF_WORDS);
      gSpi.inst = gInst;
      lcdtap::pico2::spiSlaveRegisterIrq(&gSpi);
      break;
    }
    case lcdtap::BusType::SPI_3LINE: {
      gSpi.progOffset = pio_add_program(SPI_PIO, &spi_3line_mode0_program);
      gSpi.pioProgram = &spi_3line_mode0_program;
      spi3lineSlaveInit(gSpi.progOffset);
      lcdtap::pico2::spiSlaveInitDma(&gSpi);
      gpio_set_irq_enabled(PIN_SPI_CS, GPIO_IRQ_EDGE_RISE, true);
      break;
    }
    case lcdtap::BusType::PARALLEL: {
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
// Flash-safe saveConfig wrapper
// Pauses Core 1 at a safe boundary before the flash write so that Core 1
// cannot access flash-resident code (fillScanline, OSD) during erase/program.
// =============================================================================
static void saveConfigSafe(const ConfigFile &cfg) {
  gVideo->flashAcquire(gVideoState);
  saveConfig(cfg);
  gVideo->flashRelease(gVideoState);
}

// =============================================================================
// Dispatch to the active ring buffer processor
// =============================================================================
static void processInputBuf() {
  if (gCurrentIface == lcdtap::BusType::I2C)
    lcdtap::pico2::i2cSlaveProcess(&gI2c);
  else
    lcdtap::pico2::spiSlaveProcess(&gSpi);
}

// =============================================================================
// OSD overlay for Core 1 (called after LcdTap::fillScanline)
// =============================================================================
// SRAM-resident: runs per scanline on Core 1; everything it calls
// (getOutputScreenSize, Osd::fillScanline) is SRAM-resident via
// lcdtap/hot.hpp, keeping the whole per-line fill path off the flash.
static void __not_in_flash_func(universalFillScanline)(uint16_t scanY,
                                                       uint16_t *buf,
                                                       void * /*userData*/) {
  uint16_t screenW, screenH;
  gInst->getOutputScreenSize(&screenW, &screenH);
  uint16_t yStart = (screenH - lcdtap::OSD_HEIGHT) / 2;
  uint16_t yEnd = yStart + lcdtap::OSD_HEIGHT;
  uint16_t xStart = (screenW - lcdtap::OSD_WIDTH) / 2;
  if (yStart <= scanY && scanY < yEnd) {
    gOsd.fillScanline(scanY - yStart, buf + xStart);
  }
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

  // Right button selects alt 720p timing (319.2 MHz CVT-RB) for monitors
  // that reject the standard 480 MHz timing.
  const struct dvi_timing *timing;
  if (dvi720p) {
    timing = !gpio_get(PIN_KEY_RIGHT) ? &dvi_timing_1280x720p_reduced_30hz_alt
                                      : &dvi_timing_1280x720p_reduced_30hz;
  } else {
    timing = &dvi_timing_640x480p_60hz;
  }

  // -------------------------------------------------------------------------
  // 1b. Read flash config — check for USB mass storage boot mode
  // -------------------------------------------------------------------------
  ConfigFile savedCfg = {};
  bool hasSavedCfg;
  if (!gpio_get(PIN_KEY_LEFT)) {
    // If left key is pressed at boot time, start with default settings
    hasSavedCfg = false;
  } else {
    hasSavedCfg = loadConfig(&savedCfg);
  }

  // -------------------------------------------------------------------------
  // 1c. Resolve the video backend.
  //     Must happen before clock init: NTSC and PAL each need their own
  //     clk_sys so that the sample rate is an exact integer division of it.
  // -------------------------------------------------------------------------
  if (hasSavedCfg) {
    // Sanitize against a config written by an older build, a hand-crafted
    // flash image, or a bus/output combination that is no longer legal.
    gOutputIf = outputInterfaceSanitize(
        static_cast<OutputInterface>(savedCfg.outputInterface),
        savedCfg.libConfig.busInterface);
    gCvbsDac = compositeDacSanitize(
        static_cast<CompositeDacKind>(savedCfg.compositeDac),
        savedCfg.libConfig.busInterface);
  }

  const lcdtap::pico2::CompositeTiming *cvbsTiming = nullptr;
  if (gOutputIf == OutputInterface::NTSC) {
    cvbsTiming = &lcdtap::pico2::COMPOSITE_TIMING_NTSC_J_240P;
  } else if (gOutputIf == OutputInterface::PAL) {
    cvbsTiming = &lcdtap::pico2::COMPOSITE_TIMING_PAL_B_288P;
  }

  // -------------------------------------------------------------------------
  // 2. Clock init
  //    DVI-D : PLL_USB → clk_sys=312MHz; PLL_SYS → clk_hstx=bit_clk/2
  //    NTSC  : PLL_SYS → clk_sys=315.000MHz (÷22 = 4x fsc, exact)
  //    PAL   : PLL_SYS → clk_sys=301.500MHz (÷17 = 4x fsc, +46 ppm)
  //    Composite ignores PIN_CFG_OUT_720P and the RIGHT-key timing override;
  //    those only select between DVI-D modes.
  // -------------------------------------------------------------------------
  if (cvbsTiming != nullptr) {
    lcdtap::pico2::compositeOutClockInit(cvbsTiming);
    gVideo = &BACKEND_COMPOSITE;
    gVideoState = &gCvbs;
  } else {
    lcdtap::pico2::hstxOutClockInit(timing);
  }

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

  // Output raster size. The saved dviWidth/dviHeight are always overridden
  // below, so this must follow the selected backend or composite would run
  // with DVI geometry.
  const uint32_t outW = cvbsTiming ? (uint32_t)cvbsTiming->hActivePixels
                                   : (uint32_t)timing->h_active_pixels;
  const uint32_t outH = cvbsTiming ? (uint32_t)cvbsTiming->vActiveLines
                                   : (uint32_t)timing->v_active_lines;

  // -------------------------------------------------------------------------
  // 6. LcdTap init
  // -------------------------------------------------------------------------
  lcdtap::LcdTapConfig cfg;
  lcdtap::getDefaultConfig(lcdtap::ControllerFamily::ST7789, &cfg);
  cfg.buffWidth = lcdW;
  cfg.buffHeight = lcdH;
  cfg.scaleMode = lcdtap::ScaleMode::FIT;
  cfg.dviWidth = static_cast<uint16_t>(outW);
  cfg.dviHeight = static_cast<uint16_t>(outH);

  if (hasSavedCfg) {
    savedCfg.libConfig.dviWidth = cfg.dviWidth;
    savedCfg.libConfig.dviHeight = cfg.dviHeight;
    cfg = savedCfg.libConfig;
    gCurrentIface = cfg.busInterface;
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
  // 10. Launch Core 1 (fillScanline + OSD overlay + video output)
  // -------------------------------------------------------------------------
  if (cvbsTiming != nullptr) {
    // Hardware safety: in PARALLEL mode GPIO3-11 are data/DC lines driven by
    // the external host controller, and both DACs live inside that range
    // (the R-2R ladder on GPIO5-11, the PWM pin on GPIO10 = D[7]), so
    // neither may be enabled there. outputInterfaceSanitize() already covers
    // this; the check stays because the failure mode is an output-vs-output
    // conflict, not a blank screen.
    if (gCurrentIface == lcdtap::BusType::PARALLEL) {
      panic("composite output is not available on the parallel bus");
    }
    const lcdtap::pico2::CompositeDacProfile *dac =
        (gCvbsDac == CompositeDacKind::R2R)
            ? &lcdtap::pico2::COMPOSITE_DAC_R2R_GPIO5
            : &lcdtap::pico2::COMPOSITE_DAC_PWM_GPIO10;
    lcdtap::pico2::CompositeOutConfig cvbsCfg = {
        PIN_LED, LED_TOGGLE_FRAMES, dac,
        static_cast<lcdtap::pico2::CompositeChromaMode>(CVBS_CHROMA_MODE)};
    if (!lcdtap::pico2::compositeOutInit(&gCvbs, cvbsTiming, &inst,
                                         universalFillScanline, nullptr,
                                         cvbsCfg)) {
      panic("composite output init failed");
    }
    lcdtap::pico2::compositeOutLaunchCore1(&gCvbs);
  } else {
    lcdtap::pico2::HstxOutConfig hstxCfg = {
        PIN_LED, LED_TOGGLE_FRAMES, lcdtap::pico2::HSTX_PICO_SOCK_PINOUT};
    lcdtap::pico2::hstxOutInit(&gHstx, timing, &inst, universalFillScanline,
                               nullptr, hstxCfg);
    lcdtap::pico2::hstxOutLaunchCore1(&gHstx);
  }

  // -------------------------------------------------------------------------
  // 11. USB CDC serial interface
  // -------------------------------------------------------------------------
  uartIfInit(&inst, &gCurrentIface, &gOutputIf, &gCvbsDac, switchInterface,
             saveConfigSafe);

  // -------------------------------------------------------------------------
  // 12. Main loop (Core 0)
  //     Core 1 handles fillScanline + video output; Core 0 drains the
  //     SPI/I2C ring buffers continuously without timer preemption.
  //     consumeNewFrame returns true once per frame boundary.
  // -------------------------------------------------------------------------
  while (true) {
    processInputBuf();
    uartIfProcess();

    // A UART client changed the output interface; reboot once the response
    // has been flushed so the host actually sees the acknowledgement.
    if (uartIfRebootPending() && uartIfRespIdle()) {
      sleep_ms(20);
      watchdog_reboot(0, 0, 10);
      while (true) tight_loop_contents();
    }

    if (gVideo->consumeNewFrame(gVideoState)) {
      uint64_t nowMs =
          static_cast<uint64_t>(to_ms_since_boot(get_absolute_time()));
      uint8_t action = gOsd.update(nowMs, *gInst, readKeys());
      if (action == lcdtap::OSD_ACTION_APPLY) {
        const lcdtap::OsdMenuItem *ifaceItem = nullptr;
        uint16_t id = lcdtap::OSD_ITEM_ID_SYS_BASE +
                      static_cast<uint16_t>(lcdtap::ConfigId::BUS_INTERFACE);
        gOsd.getItemById(id, &ifaceItem);
        lcdtap::BusType newIface =
            ifaceItem ? static_cast<lcdtap::BusType>(ifaceItem->config.value)
                      : gCurrentIface;

        const lcdtap::OsdMenuItem *outIfItem = nullptr;
        gOsd.getItemById(OSD_ITEM_ID_OUTPUT_IF, &outIfItem);
        OutputInterface newOutIf =
            outIfItem ? static_cast<OutputInterface>(outIfItem->config.value)
                      : gOutputIf;
        const lcdtap::OsdMenuItem *dacItem = nullptr;
        gOsd.getItemById(OSD_ITEM_ID_CVBS_DAC, &dacItem);
        CompositeDacKind newDac =
            dacItem ? static_cast<CompositeDacKind>(dacItem->config.value)
                    : gCvbsDac;
        // Items retain their value while greyed out, so a switch to a bus
        // that forbids the current selection must not carry it into flash.
        // Order matters: the DAC is clamped against the already-clamped bus.
        newOutIf = outputInterfaceSanitize(newOutIf, newIface);
        newDac = compositeDacSanitize(newDac, newIface);

        const bool busChanged = (newIface != gCurrentIface);
        const bool outIfChanged = (newOutIf != gOutputIf);
        const bool dacChanged = (newDac != gCvbsDac);

        ConfigFile toSave = {};
        toSave.libConfig = gInst->getConfig();
        toSave.outputInterface = static_cast<uint8_t>(newOutIf);
        toSave.compositeDac = static_cast<uint8_t>(newDac);
        saveConfigSafe(toSave);

        // Reboot when the output interface changes, because clk_sys differs
        // between all three modes. Also reboot when the bus or the DAC
        // changes while composite is running: the DAC is bound to its pins
        // and peripheral at init, and the R-2R ladder covers the I2C pins,
        // so handing GPIO8/9 to the I2C driver while the PIO still drives
        // them would be a pin conflict. A DAC change with DVI-D selected is
        // inert and only needs persisting.
        if (outIfChanged || (outputInterfaceIsComposite(newOutIf) &&
                             (busChanged || dacChanged))) {
          watchdog_reboot(0, 0, 50);
          while (true) tight_loop_contents();
        }
        if (busChanged) {
          switchInterface(newIface);
        }
        gCvbsDac = newDac;
      }
    }
  }
}
