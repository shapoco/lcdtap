// LcdTap pico2w_remote — capture LCD controller traffic on a Raspberry Pi
// Pico 2 W and serve the framebuffer over WiFi (HTTP JSON API + web UI).
//
// Core split (opposite of pico2_universal, which needs Core 1 for video):
//   Core 1 — input drain: SPI/I2C/parallel ring buffers -> LcdTap. The bus
//            IRQs are registered here so they land on Core 1's NVIC and the
//            62.5 MHz SPI stream is never delayed by lwIP work.
//   Core 0 — cyw43/lwIP poll, HTTP server, USB CDC JSON interface, LED.
//
// Cross-core requests (flash write parking, bus switching) go through
// mailbox flags checked every drain-loop iteration.

#include <cstdint>
#include <cstring>

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "lcdtap/lcdtap.hpp"
#include "lcdtap/pico2/bus_input.hpp"
#include "lcdtap/pico2/cdc_trx.hpp"
#include "lcdtap/pico2/flash_store.hpp"
#include "lcdtap/pico2/json_intf.hpp"
#include "lcdtap/pico2/splash.hpp"
#include "lcdtap/pico2/sys_clock.hpp"

#include "config.h"
#include "http_server.hpp"
#include "led_status.hpp"
#include "net_cmds.hpp"
#include "net_config.hpp"
#include "stats.hpp"
#include "wifi_mgr.hpp"

#include "parallel_8bit.pio.h"
#include "spi_3line_mode0.pio.h"
#include "spi_4line_mode0.pio.h"

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
// =============================================================================
static uint8_t memPool[MEM_POOL_SIZE];

static void *poolAlloc(size_t size) {
  size = (size + 3u) & ~3u;
  if (size > sizeof(memPool)) return nullptr;
  return memPool;
}

static void poolFree(void *ptr) { (void)ptr; }

// =============================================================================
// Ring buffers
// =============================================================================
static uint32_t
    __attribute__((aligned(SPI_RING_BUF_BYTES))) spiRingBuf[SPI_RING_BUF_WORDS];
static uint32_t i2cRingBuf[I2C_RING_BUF_WORDS];

// =============================================================================
// Module state
// =============================================================================
static lcdtap::BusType gCurrentIface = lcdtap::BusType::SPI_4LINE;
static bool gIfaceActive = false;
static lcdtap::LcdTap *gInst = nullptr;
static lcdtap::pico2::SpiSlaveState gSpi;
static lcdtap::pico2::I2cSlaveState gI2c;
static NetConfig gNetCfg;

static JsonIntf gCdcIntf;
static JsonIntf gHttpIntf;

// Device config: LcdTap settings only (network settings live in their own
// sector, see net_config).
struct DeviceConfigFile {
  lcdtap::LcdTapConfig libConfig;
  uint8_t reserved[4];
};

static const lcdtap::pico2::BusInputContext kBusCtx = {
    &gSpi,
    &gI2c,
    {i2c0, PIN_I2C_SDA, PIN_I2C_SCL, I2C_SLAVE_ADDR},
    i2cRingBuf,
    I2C_RING_BUF_WORDS,
    PIN_PAR_WR,
    PIN_PAR_DATA_BASE,
};

// =============================================================================
// GPIO interrupt handler (RST pin; CS pin for SPI/Parallel modes).
// Registered on Core 1.
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
// Core 1: input drain loop + mailboxes
// =============================================================================
static volatile uint32_t gFlashParkReq = 0;
static volatile uint32_t gFlashParkAck = 0;
static volatile uint32_t gBusSwitchReq = 0;  // BusType + 1; 0 = none
static volatile bool gCore1Ready = false;

// Park in SRAM with IRQs off while Core 0 erases/programs flash; anything
// running from flash here would fetch from a dead XIP window.
static void __not_in_flash_func(core1FlashParkLoop)() {
  uint32_t ints = save_and_disable_interrupts();
  gFlashParkAck = 1;
  __dmb();
  while (gFlashParkReq) tight_loop_contents();
  gFlashParkAck = 0;
  __dmb();
  restore_interrupts(ints);
}

static void core1Main() {
  // Bus IRQ handlers (I2C, CS/RST GPIO) bind to the calling core's NVIC —
  // they must live on Core 1 so input capture never waits on Core 0.
  gpio_set_irq_enabled_with_callback(PIN_RST,
                                     GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                     /*enabled=*/true, &gpioIrqHandler);
  irq_set_priority(IO_IRQ_BANK0, 0x00);

  lcdtap::pico2::busInputSwitch(kBusCtx, gInst, &gCurrentIface, &gIfaceActive,
                                gCurrentIface);
  __dmb();
  gCore1Ready = true;

  while (true) {
    lcdtap::pico2::busInputProcess(kBusCtx, gCurrentIface);
    if (gFlashParkReq) core1FlashParkLoop();
    if (gBusSwitchReq) {
      lcdtap::BusType next = static_cast<lcdtap::BusType>(gBusSwitchReq - 1u);
      lcdtap::pico2::busInputSwitch(kBusCtx, gInst, &gCurrentIface,
                                    &gIfaceActive, next);
      __dmb();
      gBusSwitchReq = 0;
    }
  }
}

// =============================================================================
// Core 0 -> Core 1 requests
// =============================================================================

// Park Core 1, write one flash blob, resume. The whole handshake is bounded
// by Core 1's drain-loop iteration time (microseconds).
static void flashSaveParked(uint32_t sectorsFromEnd, const void *data,
                            size_t len) {
  gFlashParkReq = 1;
  __dmb();
  while (!gFlashParkAck) tight_loop_contents();
  lcdtap::pico2::flashStoreSave(sectorsFromEnd, data, len);
  gFlashParkReq = 0;
  __dmb();
  while (gFlashParkAck) tight_loop_contents();
}

static void requestBusSwitch(lcdtap::BusType next) {
  gBusSwitchReq = static_cast<uint32_t>(next) + 1u;
  __dmb();
  while (gBusSwitchReq) tight_loop_contents();
}

static void saveDeviceConfig() {
  DeviceConfigFile f = {};
  f.libConfig = gInst->getConfig();
  flashSaveParked(DEVICE_CONFIG_SECTORS_FROM_END, &f, sizeof(f));
}

static void saveNetConfigParked(const NetConfig &cfg) {
  flashSaveParked(NET_CONFIG_SECTORS_FROM_END, &cfg, sizeof(cfg));
}

// =============================================================================
// JSON interface callbacks (no host params — remote has no video settings)
// =============================================================================

static bool remoteCommitParams(const lcdtap::LcdTapConfig &cfg,
                               lcdtap::BusType oldBus, void * /*ctx*/) {
  // All buses run at the same clock here, so a bus change never needs a
  // reboot — hand it to Core 1 and persist.
  if (cfg.busInterface != oldBus) requestBusSwitch(cfg.busInterface);
  saveDeviceConfig();
  return false;
}

static int remoteStatsCollect(lcdtap::StatEntry *out, int maxCount,
                              void * /*ctx*/) {
  return statsCollect(out, maxCount);
}

static void remoteStatsReset(void * /*ctx*/) { statsReset(); }

static JsonIntfCallbacks remoteCallbacks() {
  JsonIntfCallbacks cb;
  cb.commitParams = remoteCommitParams;
  cb.statsCollect = remoteStatsCollect;
  cb.statsReset = remoteStatsReset;
  cb.execAppCommand = netCmdsExec;
  return cb;
}

// =============================================================================
// main
// =============================================================================
int main() {
  // Same proven 312 MHz configuration as pico2_universal's DVI mode: the
  // 62.5 MHz SPI capture needs the headroom, and clk_usb stays at 48 MHz.
  // The CYW43 PIO clock divider is compensated via CYW43_PIO_CLOCK_DIV_INT.
  lcdtap::pico2::sysClockInit312();

  // -------------------------------------------------------------------------
  // RST input (pull-up; active low from the SPI/Parallel master)
  // -------------------------------------------------------------------------
  gpio_init(PIN_RST);
  gpio_set_dir(PIN_RST, GPIO_IN);
  gpio_pull_up(PIN_RST);

  // -------------------------------------------------------------------------
  // Load configs
  // -------------------------------------------------------------------------
  DeviceConfigFile savedCfg = {};
  const bool hasSavedCfg = lcdtap::pico2::flashStoreLoad(
      DEVICE_CONFIG_SECTORS_FROM_END, &savedCfg, sizeof(savedCfg));
  netConfigLoad(&gNetCfg);

  // -------------------------------------------------------------------------
  // LcdTap init
  // -------------------------------------------------------------------------
  lcdtap::LcdTapConfig cfg;
  lcdtap::getDefaultConfig(lcdtap::ControllerFamily::ST7789, &cfg);
  cfg.buffWidth = LCDTAP_LCD_SIZE_W;
  cfg.buffHeight = LCDTAP_LCD_SIZE_H;
  cfg.scaleMode = lcdtap::ScaleMode::FIT;
  // Nominal output raster: there is no video output, but the library derives
  // its output-source region (used by getframebuffer) from these.
  cfg.dviWidth = 1280;
  cfg.dviHeight = 720;

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

  lcdtap::pico2::drawSplash(gInst->getFramebuf(), cfg.buffWidth, cfg.buffHeight,
                            cfg.outputRotation);
  gInst->setDisplayOn(true);

  // -------------------------------------------------------------------------
  // Pre-initialize gSpi common fields (used by every SPI-variant bus mode)
  // -------------------------------------------------------------------------
  gSpi.cfg = {SPI_PIO,      SPI_SM,     PIN_SPI_CS,       PIN_SPI_SCLK,
              PIN_SPI_MOSI, PIN_SPI_DC, SPI_RING_BUF_LOG2};
  gSpi.ringBuf = spiRingBuf;
  gSpi.ringWords = SPI_RING_BUF_WORDS;
  gSpi.inst = &inst;
  gSpi.dmaCh = -1;
  gSpi.pioProgram = nullptr;

  StatsSources statsSrc = {&inst, &gCurrentIface, &gSpi, &gI2c};
  statsInit(statsSrc);

  // -------------------------------------------------------------------------
  // Launch Core 1: bus init + IRQ registration + drain loop
  // -------------------------------------------------------------------------
  multicore_launch_core1(core1Main);
  while (!gCore1Ready) tight_loop_contents();

  // -------------------------------------------------------------------------
  // USB CDC JSON interface (also used for WiFi setup)
  // -------------------------------------------------------------------------
  netCmdsInit(&gNetCfg, saveNetConfigParked);
  cdcTrxInit();
  jsonIntfInit(&gCdcIntf, &inst, &gCurrentIface, cdcTrxTransport(),
               remoteCallbacks());

  // -------------------------------------------------------------------------
  // WiFi + HTTP + LED
  // -------------------------------------------------------------------------
  const bool wifiOk = wifiMgrInit(&gNetCfg);

  jsonIntfInit(&gHttpIntf, &inst, &gCurrentIface, httpServerTransport(),
               remoteCallbacks());
  httpServerInit(&gHttpIntf);

  // -------------------------------------------------------------------------
  // Main loop (Core 0)
  // -------------------------------------------------------------------------
  while (true) {
    const uint64_t nowMs =
        static_cast<uint64_t>(to_ms_since_boot(get_absolute_time()));

    if (wifiOk) {
      cyw43_arch_poll();
      wifiMgrProcess(nowMs);
      switch (wifiMgrState()) {
        case WifiState::UNCONFIGURED:
          ledStatusSetPattern(LedPattern::UNCONFIGURED);
          break;
        case WifiState::CONNECTING:
          ledStatusSetPattern(LedPattern::CONNECTING);
          break;
        case WifiState::CONNECTED:
          ledStatusSetPattern(LedPattern::CONNECTED);
          break;
        case WifiState::FAILED: ledStatusSetPattern(LedPattern::FAILED); break;
      }
      ledStatusTick(nowMs);
      httpServerProcess();
    }

    jsonIntfProcess(&gCdcIntf);
    statsTick(nowMs);

    // A client changed settings that only apply at boot (setnetconfig).
    // Reboot once every queued response has drained and no HTTP API
    // connection would be cut off.
    const bool cdcWants =
        jsonIntfRebootPending(&gCdcIntf) && jsonIntfRespIdle(&gCdcIntf);
    const bool httpWants =
        jsonIntfRebootPending(&gHttpIntf) && jsonIntfRespIdle(&gHttpIntf);
    if ((cdcWants || httpWants) && !httpServerApiBusy()) {
      sleep_ms(20);
      watchdog_reboot(0, 0, 10);
      while (true) tight_loop_contents();
    }
  }
}
