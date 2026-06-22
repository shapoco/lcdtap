#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <hardware/gpio.h>
#include <pico/stdlib.h>

#include "lcdtap/lcdtap.hpp"
#include "lcdtap/pico2/hstx_out.hpp"
#include "lcdtap/pico2/i2c_slave.hpp"
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
// I2C ring buffer  (same word format as SPI ring buffer)
// =============================================================================
static uint32_t i2cRingBuf[I2C_RING_BUF_WORDS];

// =============================================================================
// Module state
// =============================================================================
static lcdtap::LcdTap *gInst = nullptr;
static lcdtap::pico2::SpiSlaveState gSpi;
static lcdtap::pico2::I2cSlaveState gI2c;
static lcdtap::pico2::HstxOutState gHstx;

// =============================================================================
// GPIO interrupt handler (RST and CS pins, SPI mode)
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
  for (uint pin : {PIN_CFG_IFACE_SEL, PIN_CFG_OUT_720P, PIN_CFG_LCD_SIZE_SEL,
                   PIN_CFG_ROT0, PIN_CFG_ROT1}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
  }
  sleep_ms(1);

  const bool useI2C =
      gpio_get(PIN_CFG_IFACE_SEL);  // HIGH=I2C (default), LOW=SPI
  const bool dvi720p =
      !gpio_get(PIN_CFG_OUT_720P);  // LOW=720p, HIGH=480p (default)
  const bool useSz2 =
      !gpio_get(PIN_CFG_LCD_SIZE_SEL);  // LOW=Size2, HIGH=Size1 (default)
  const int rot = static_cast<int>((!gpio_get(PIN_CFG_ROT1) ? 2u : 0u) |
                                   (!gpio_get(PIN_CFG_ROT0) ? 1u : 0u));

  const struct dvi_timing *timing =
      dvi720p ? &dvi_timing_1280x720p_reduced_30hz : &dvi_timing_640x480p_60hz;

  // -------------------------------------------------------------------------
  // 2. Clock init (PLL_USB → clk_sys=288MHz; PLL_SYS → clk_hstx=bit_clk/2)
  // -------------------------------------------------------------------------
  lcdtap::pico2::hstxOutClockInit(timing);

  // -------------------------------------------------------------------------
  // 3. LED
  // -------------------------------------------------------------------------
  gpio_init(PIN_LED);
  gpio_set_dir(PIN_LED, GPIO_OUT);
  gpio_put(PIN_LED, 0);

  const uint32_t dviW = (uint32_t)timing->h_active_pixels;
  const uint32_t dviH = (uint32_t)timing->v_active_lines;

  // -------------------------------------------------------------------------
  // 5. LcdTap init (SSD1306)
  // -------------------------------------------------------------------------
  const uint16_t lcdW = useSz2 ? LCDTAP_LCD_SIZE2_W : LCDTAP_LCD_SIZE1_W;
  const uint16_t lcdH = useSz2 ? LCDTAP_LCD_SIZE2_H : LCDTAP_LCD_SIZE1_H;

  lcdtap::LcdTapConfig cfg;
  lcdtap::getDefaultConfig(lcdtap::ControllerFamily::SSD1306, &cfg);
  cfg.buffWidth = lcdW;
  cfg.buffHeight = lcdH;
  cfg.scaleMode = lcdtap::ScaleMode::FIT;
  cfg.inverted = false;  // fixed
  cfg.dviWidth = static_cast<uint16_t>(dviW);
  cfg.dviHeight = static_cast<uint16_t>(dviH);

  lcdtap::HostInterface host;
  host.alloc = poolAlloc;
  host.free = poolFree;
  host.log = nullptr;
  host.userData = nullptr;

  lcdtap::LcdTap inst(cfg, host);
  if (inst.getStatus() != lcdtap::Status::OK) panic("LcdTap init failed");

  gInst = &inst;
  inst.setOutputRotation(rot);

  // -------------------------------------------------------------------------
  // 6. Input peripheral init
  // -------------------------------------------------------------------------
  if (useI2C) {
    lcdtap::pico2::I2cSlaveConfig i2cCfg = {i2c0, PIN_I2C_SDA, PIN_I2C_SCL,
                                            I2C_SLAVE_ADDR};
    lcdtap::pico2::i2cSlaveInit(&gI2c, i2cCfg, i2cRingBuf, I2C_RING_BUF_WORDS);
    gI2c.inst = &inst;
  } else {
    // RST: interrupt on both edges (reset assert / release)
    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_IN);
    gpio_pull_up(PIN_RST);
    gpio_set_irq_enabled_with_callback(PIN_RST,
                                       GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                       /*enabled=*/true, &gpioIrqHandler);
    irq_set_priority(IO_IRQ_BANK0, 0x00);

    // SPI slave PIO + DMA (after dvi_init so DVI claims its channels first)
    lcdtap::pico2::SpiSlaveConfig spiCfg = {
        SPI_PIO,      SPI_SM,     PIN_SPI_CS,       PIN_SPI_SCLK,
        PIN_SPI_MOSI, PIN_SPI_DC, SPI_RING_BUF_LOG2};
    lcdtap::pico2::spiSlaveInit(&gSpi, spiCfg, spiRingBuf, SPI_RING_BUF_WORDS);
    gSpi.inst = &inst;
    lcdtap::pico2::spiSlaveRegisterIrq(&gSpi);
  }

  // -------------------------------------------------------------------------
  // 7. Launch Core 1 (fillScanline + HSTX DVI output)
  // -------------------------------------------------------------------------
  lcdtap::pico2::HstxOutConfig hstxCfg = {PIN_LED, LED_TOGGLE_FRAMES,
                                          lcdtap::pico2::HSTX_PICO_SOCK_PINOUT};
  lcdtap::pico2::hstxOutInit(&gHstx, timing, &inst, nullptr, nullptr, hstxCfg);
  lcdtap::pico2::hstxOutLaunchCore1(&gHstx);

  // -------------------------------------------------------------------------
  // 8. Main loop (Core 0)
  //    Core 1 handles fillScanline + HSTX DVI output; Core 0 drains the
  //    SPI/I2C ring buffers continuously without timer preemption.
  //    hstxOutConsumeNewFrame returns true once per frame boundary.
  // -------------------------------------------------------------------------
  int currentRot = rot;

  while (true) {
    if (useI2C)
      lcdtap::pico2::i2cSlaveProcess(&gI2c);
    else
      lcdtap::pico2::spiSlaveProcess(&gSpi);

    if (lcdtap::pico2::hstxOutConsumeNewFrame(&gHstx)) {
      int newRot = static_cast<int>((!gpio_get(PIN_CFG_ROT1) ? 2u : 0u) |
                                    (!gpio_get(PIN_CFG_ROT0) ? 1u : 0u));
      if (newRot != currentRot) {
        currentRot = newRot;
        gInst->setOutputRotation(currentRot);
      }
    }
  }
}
