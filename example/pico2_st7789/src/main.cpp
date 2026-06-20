#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#include "common_dvi_pin_configs.h"
#include "dvi.h"
#include "dvi_timing.h"

#include "lcdtap/lcdtap.hpp"
#include "lcdtap/pico2/dvi_out.hpp"
#include "lcdtap/pico2/spi_slave.hpp"

#include "config.h"

static_assert(PIN_SPI_CS == SPI_CS_PIN,
              "PIN_SPI_CS mismatch with spi_4line_mode0.pio");
static_assert(PIN_SPI_SCLK == SPI_SCLK_PIN,
              "PIN_SPI_SCLK mismatch with spi_4line_mode0.pio");
static_assert(PIN_SPI_MOSI + 1u == PIN_SPI_DC,
              "DC must be MOSI+1 for 'in pins, 2'");

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
// SPI ring buffer  (word = [bit8: DC, bits7:0: data byte])
// DMA alignment constraint: must be aligned to its own byte size.
// =============================================================================
static uint32_t
    __attribute__((aligned(SPI_RING_BUF_BYTES))) spiRingBuf[SPI_RING_BUF_WORDS];

// =============================================================================
// DVI
// =============================================================================
static struct dvi_inst dvi0;

static uint16_t
    __attribute__((aligned(4))) scanlineBufs[N_SCANLINE_BUFS][DVI_MAX_W];

// =============================================================================
// Module state
// =============================================================================
static lcdtap::LcdTap *gInst = nullptr;
static lcdtap::pico2::SpiSlaveState gSpi;
static lcdtap::pico2::DviOutState gDvi;

// =============================================================================
// GPIO interrupt handler  (RST pin; CS pin)
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
// main
// =============================================================================
int main() {
  // -------------------------------------------------------------------------
  // 1. Read boot-time configuration GPIOs
  //    All config pins are active-low with internal pull-ups.
  //    Default (not connected, HIGH) = primary mode; LOW = alternate mode.
  // -------------------------------------------------------------------------
  for (uint pin : {PIN_CFG_OUT_720P, PIN_CFG_LCD_SIZE_SEL, PIN_CFG_INVERTED,
                   PIN_CFG_SWAP_RB, PIN_CFG_ROT0, PIN_CFG_ROT1}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
  }
  sleep_ms(1);  // allow pull-ups to settle

  const bool dvi720p =
      !gpio_get(PIN_CFG_OUT_720P);  // LOW=720p, HIGH=480p (default)
  const bool useSz2 =
      !gpio_get(PIN_CFG_LCD_SIZE_SEL);  // LOW=Size2, HIGH=Size1 (default)
  const bool inverted =
      !gpio_get(PIN_CFG_INVERTED);  // LOW=inverted, HIGH=normal (default)
  const bool swapRB =
      !gpio_get(PIN_CFG_SWAP_RB);  // LOW=swap, HIGH=no swap (default)
  const int rot = static_cast<int>((!gpio_get(PIN_CFG_ROT1) ? 2u : 0u) |
                                   (!gpio_get(PIN_CFG_ROT0) ? 1u : 0u));

  const uint16_t lcdW = useSz2 ? LCDTAP_LCD_SIZE2_W : LCDTAP_LCD_SIZE1_W;
  const uint16_t lcdH = useSz2 ? LCDTAP_LCD_SIZE2_H : LCDTAP_LCD_SIZE1_H;

  const struct dvi_timing *timing =
      dvi720p ? &dvi_timing_1280x720p_reduced_30hz  // 319.2 MHz bit clock
              : &dvi_timing_640x480p_60hz;          // 252.0 MHz bit clock

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
  // 4. DVI init (claims DMA channels and PIO0 state machines)
  // -------------------------------------------------------------------------
  dvi0.timing = timing;
  dvi0.ser_cfg = pico_sock_cfg;  // GPIO12-19, pio0, invert_diffpairs=false
  dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

  const uint32_t dviW = timing->h_active_pixels / DVI_SYMBOLS_PER_WORD;
  const uint32_t dviH = timing->v_active_lines / DVI_VERTICAL_REPEAT;

  // -------------------------------------------------------------------------
  // 5. LcdTap init
  // -------------------------------------------------------------------------
  lcdtap::LcdTapConfig cfg;
  lcdtap::getDefaultConfig(lcdtap::ControllerFamily::ST7789, &cfg);
  cfg.buffWidth = lcdW;
  cfg.buffHeight = lcdH;
  cfg.scaleMode = lcdtap::ScaleMode::FIT;
  cfg.inverted = inverted;
  cfg.swapRB = swapRB;
  cfg.dviWidth = static_cast<uint16_t>(dviW);
  cfg.dviHeight = static_cast<uint16_t>(dviH);

  lcdtap::HostInterface host;
  host.alloc = poolAlloc;
  host.free = poolFree;
  host.log = nullptr;
  host.userData = nullptr;

  lcdtap::LcdTap inst(cfg, host);
  if (inst.getStatus() != lcdtap::Status::OK) panic("LcdTap init failed");

  gInst = &inst;  // expose to IRQ handler
  inst.setOutputRotation(rot);

  // -------------------------------------------------------------------------
  // 6. RST and CS interrupts
  // -------------------------------------------------------------------------
  gpio_init(PIN_RST);
  gpio_set_dir(PIN_RST, GPIO_IN);
  gpio_pull_up(PIN_RST);
  gpio_set_irq_enabled_with_callback(PIN_RST,
                                     GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                     /*enabled=*/true, &gpioIrqHandler);
  irq_set_priority(IO_IRQ_BANK0, 0x00);

  // -------------------------------------------------------------------------
  // 7. SPI slave PIO + DMA
  //    Must come after dvi_init() so DVI claims its DMA channels first.
  // -------------------------------------------------------------------------
  lcdtap::pico2::SpiSlaveConfig spiCfg = {
      SPI_PIO,      SPI_SM,     PIN_SPI_CS,       PIN_SPI_SCLK,
      PIN_SPI_MOSI, PIN_SPI_DC, SPI_RING_BUF_LOG2};
  lcdtap::pico2::spiSlaveInit(&gSpi, spiCfg, spiRingBuf, SPI_RING_BUF_WORDS);
  gSpi.inst = &inst;
  lcdtap::pico2::spiSlaveRegisterIrq(&gSpi);

  // -------------------------------------------------------------------------
  // 8. Launch Core 1 (fillScanline + TMDS encode + serialise)
  // -------------------------------------------------------------------------
  lcdtap::pico2::DviOutConfig dviCfg = {PIN_LED, LED_TOGGLE_FRAMES};
  lcdtap::pico2::dviOutPrepare(&gDvi, &dvi0, scanlineBufs[0],
                               sizeof(scanlineBufs[0]), &inst, nullptr, nullptr,
                               dviCfg);
  gDvi.dviH = dviH;
  lcdtap::pico2::dviOutLaunchCore1(&gDvi);

  // -------------------------------------------------------------------------
  // 9. Main loop (Core 0)
  //    Core 1 handles fillScanline + TMDS encode; Core 0 is free to drain
  //    the SPI ring buffer continuously without timer preemption.
  //    dviOutConsumeNewFrame returns true once per frame boundary.
  // -------------------------------------------------------------------------
  int currentRot = rot;

  while (true) {
    lcdtap::pico2::spiSlaveProcess(&gSpi);
    if (lcdtap::pico2::dviOutConsumeNewFrame(&gDvi)) {
      int newRot = static_cast<int>((!gpio_get(PIN_CFG_ROT1) ? 2u : 0u) |
                                    (!gpio_get(PIN_CFG_ROT0) ? 1u : 0u));
      if (newRot != currentRot) {
        currentRot = newRot;
        gInst->setOutputRotation(currentRot);
      }
    }
  }
}
