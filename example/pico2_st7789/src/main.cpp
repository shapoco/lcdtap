#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "common_dvi_pin_configs.h"  // pico_sock_cfg
#include "dvi.h"
#include "dvi_timing.h"

#include "lcdtap/lcdtap.hpp"

#include "config.h"
#include "spi_4line_mode0.pio.h"

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
// Ring buffer  (word = [bit8: DC, bits7:0: data byte])
// =============================================================================
static uint32_t
    __attribute__((aligned(SPI_RING_BUF_BYTES))) spiRingBuf[SPI_RING_BUF_WORDS];

static int spiDmaCh = -1;
static uint32_t spiReadIdx = 0;  // index into spiRingBuf[]

// =============================================================================
// DVI
// =============================================================================
static struct dvi_inst dvi0;

// Static RGB565 scanline buffers, recycled via q_colour_free.
// Sized to DVI_MAX_W so they fit any supported DVI timing without allocation.
static uint16_t scanlineBufs[N_SCANLINE_BUFS][DVI_MAX_W];

// =============================================================================
// LcdTap instance (set after init so the RST callback can reach it)
// =============================================================================
static lcdtap::LcdTap *gInst = nullptr;

// PIO program offset — stored so gpioIrqHandler can reset the SM PC on CS
// de-assertion (pio_sm_restart does not reset the PC).
static uint gSpiProgOffset = 0u;

// =============================================================================
// Reset PIO State Machine
// =============================================================================
static void resetPioSm() {
  pio_sm_set_enabled(SPI_PIO, SPI_SM, false);
  pio_sm_clear_fifos(SPI_PIO, SPI_SM);
  pio_sm_restart(SPI_PIO, SPI_SM);
  pio_sm_exec(SPI_PIO, SPI_SM, pio_encode_jmp(gSpiProgOffset));
  pio_sm_set_enabled(SPI_PIO, SPI_SM, true);
}

// =============================================================================
// GPIO interrupt handler  (RST pin; CS pin)
// =============================================================================
static void gpioIrqHandler(uint gpio, uint32_t events) {
  if (gpio == PIN_RST && gInst) {
    if (events & GPIO_IRQ_EDGE_FALL) {
      gInst->inputReset(true);
      resetPioSm();
    }
    gInst->inputReset(!gpio_get(PIN_RST));
  }
  if (gpio == PIN_SPI_CS && (events & GPIO_IRQ_EDGE_RISE)) {
    // CS rising edge: transaction ended or aborted.  Reset the SM so any
    // partial byte is discarded and it is ready for the next transaction.
    resetPioSm();
  }
}

// =============================================================================
// Core 1: DVI TMDS encode + serialise (never returns)
// =============================================================================
static void core1Main() {
  // Route DVI DMA IRQ to this core; use DMA_IRQ_0 (highest priority).
  dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);

  // Wait until Core 0 has queued the first scanline before starting output.
  while (queue_is_empty(&dvi0.q_colour_valid)) __wfe();

  dvi_start(&dvi0);
  dvi_scanbuf_main_16bpp(&dvi0);  // infinite loop
}

// =============================================================================
// SPI slave init  (PIO1 SM0)
// =============================================================================
static void spiSlaveInit(uint prog_offset) {
  for (uint pin : {PIN_SPI_SCLK, PIN_SPI_MOSI, PIN_SPI_DC}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
  }
  gpio_init(PIN_SPI_CS);
  gpio_set_dir(PIN_SPI_CS, GPIO_IN);
  gpio_pull_up(PIN_SPI_CS);

  pio_sm_config c = spi_4line_mode0_program_get_default_config(prog_offset);
  sm_config_set_in_pins(&c, PIN_SPI_MOSI);  // IN_BASE=GPIO3; DC is IN_BASE+1
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
  channel_config_set_read_increment(&cfg, false);  // fixed: PIO RX FIFO
  channel_config_set_write_increment(&cfg, true);  // advances through ring
  channel_config_set_dreq(&cfg, pio_get_dreq(SPI_PIO, SPI_SM, /*is_tx=*/false));
  channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
  // Wrap the write address at SPI_RING_BUF_BYTES boundary → circular buffer.
  channel_config_set_ring(&cfg, /*write=*/true, SPI_RING_BUF_LOG2);

  dma_channel_configure((uint)spiDmaCh, &cfg,
                        spiRingBuf,             // initial write address
                        &SPI_PIO->rxf[SPI_SM],  // read address: PIO RX FIFO
                        0xFFFFFFFFu,            // run forever
                        /*trigger=*/true);
}

// =============================================================================
// Drain the ring buffer and feed bytes to LcdTap
// =============================================================================
static void processSpiRingBuf() {
  // Current write position from the DMA controller.
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
      // command byte
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

  const uint32_t dviW = timing->h_active_pixels / 2;
  const uint32_t dviH = timing->v_active_lines / DVI_VERTICAL_REPEAT;

  for (int i = 0; i < N_SCANLINE_BUFS; ++i) {
    uint16_t *p = scanlineBufs[i];
    queue_add_blocking_u32(&dvi0.q_colour_free, &p);
  }

  // -------------------------------------------------------------------------
  // 5. LcdTap init
  // -------------------------------------------------------------------------
  lcdtap::LcdTapConfig cfg;
  lcdtap::getDefaultConfig(lcdtap::ControllerType::ST7789, &cfg);
  cfg.lcdWidth = lcdW;
  cfg.lcdHeight = lcdH;
  cfg.scaleMode = lcdtap::ScaleMode::FIT;
  cfg.invertInvPolarity = inverted;
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

  inst.setOutputRotation(rot);  // apply boot-time rotation setting

  // -------------------------------------------------------------------------
  // 6. RST and CS interrupts
  // -------------------------------------------------------------------------
  // RST: interrupt on both edges (reset assert / release)
  gpio_init(PIN_RST);
  gpio_set_dir(PIN_RST, GPIO_IN);
  gpio_pull_up(PIN_RST);
  gpio_set_irq_enabled_with_callback(PIN_RST,
                                     GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                     /*enabled=*/true, &gpioIrqHandler);
  // CS: rising edge only — resets the PIO SM on transaction end/abort.
  // Shares the callback already registered for RST above.
  gpio_set_irq_enabled(PIN_SPI_CS, GPIO_IRQ_EDGE_RISE, true);

  // -------------------------------------------------------------------------
  // 7. SPI slave PIO + DMA
  //    Must come after dvi_init() so DVI claims its DMA channels first.
  // -------------------------------------------------------------------------
  gSpiProgOffset = pio_add_program(SPI_PIO, &spi_4line_mode0_program);
  spiSlaveInit(gSpiProgOffset);
  spiDmaInit();

  // -------------------------------------------------------------------------
  // 8. Launch Core 1 for DVI TMDS output
  // -------------------------------------------------------------------------
  multicore_launch_core1(core1Main);

  // -------------------------------------------------------------------------
  // 9. Main loop (Core 0)
  // -------------------------------------------------------------------------
  uint32_t scanY = 0;
  uint32_t frame = 0;
  bool led = false;
  int currentRot = rot;

  while (true) {
    uint16_t *buf;
    while (!queue_try_remove_u32(&dvi0.q_colour_free, &buf)) {
      processSpiRingBuf();
    }

    gInst->fillScanline(static_cast<uint16_t>(scanY), buf);

    queue_add_blocking_u32(&dvi0.q_colour_valid, &buf);

    if (++scanY >= dviH) {
      scanY = 0;
      // Check rotation GPIO and update if changed
      int newRot = static_cast<int>((!gpio_get(PIN_CFG_ROT1) ? 2u : 0u) |
                                    (!gpio_get(PIN_CFG_ROT0) ? 1u : 0u));
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
