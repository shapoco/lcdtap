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
#include "par_slave.pio.h"

// =============================================================================
// Memory pool (bump allocator for LcdTap)
// =============================================================================
static uint8_t memPool[MEM_POOL_SIZE];
static size_t memPoolOffset = 0;

static void *poolAlloc(size_t size) {
  // 4-byte align
  size = (size + 3u) & ~3u;
  if (memPoolOffset + size > sizeof(memPool)) return nullptr;
  void *p = memPool + memPoolOffset;
  memPoolOffset += size;
  return p;
}

// =============================================================================
// Ring buffer  (word = [bit8: DCX, bits7:0: data byte])
// =============================================================================
static uint32_t
    __attribute__((aligned(SPI_RING_BUF_BYTES))) spiRingBuf[SPI_RING_BUF_WORDS];

static int spiDmaCh = -1;
static uint32_t spiReadIdx = 0;  // index into spiRingBuf[]

// =============================================================================
// DVI
// =============================================================================
static struct dvi_inst dvi0;

// Pre-allocated RGB565 scanline buffers, recycled via q_colour_free.
static uint16_t *scanlineBufs[N_SCANLINE_BUFS];

// =============================================================================
// LcdTap instance (set after init so the RESX callback can reach it)
// =============================================================================
static lcdtap::LcdTap *gInst = nullptr;

// =============================================================================
// GPIO interrupt handler  (RESX pin)
// =============================================================================
static void gpioIrqHandler(uint gpio, uint32_t events) {
  if (gpio == PIN_PAR_RESX && gInst) {
    gInst->inputReset((events & GPIO_IRQ_EDGE_FALL) != 0u);
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
// Parallel slave init  (PIO1 SM0)
// =============================================================================
static void parSlaveInit(uint prog_offset) {
  // BCLK and DCX are inputs.
  gpio_init(PIN_PAR_BCLK);
  gpio_set_dir(PIN_PAR_BCLK, GPIO_IN);
  gpio_init(PIN_PAR_DCX);
  gpio_set_dir(PIN_PAR_DCX, GPIO_IN);
  // D[0..7] = GPIO 4-11 are all inputs.
  for (uint pin = PIN_PAR_DATA_BASE; pin < PIN_PAR_DATA_BASE + 8u; ++pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
  }
  // RESX pulled high so the line reads "deasserted" when unconnected.
  gpio_pull_up(PIN_PAR_RESX);

  pio_sm_config c = par_slave_with_dcx_program_get_default_config(prog_offset);
  // D[0] is the IN base pin; 'in pins, 8' samples GPIO 4-11.
  sm_config_set_in_pins(&c, PIN_PAR_DATA_BASE);
  // DCX is the JMP_PIN; 'jmp pin' branches on its state.
  sm_config_set_jmp_pin(&c, PIN_PAR_DCX);
  // Shift left so D7..D0 accumulate in bits[7:0] after 8 shifts.
  // AUTOPUSH disabled; we push manually after each 9-bit word.
  sm_config_set_in_shift(&c, /*shift_direction=*/false,
                         /*autopush=*/false,
                         /*push_threshold=*/32);
  // Join both FIFOs into one 8-deep RX FIFO.
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
// static uint8_t dataBatch[DATA_BATCH_CAP];
// static size_t dataBatchLen = 0;

static void processSpiRingBuf() {
  // Current write position from the DMA controller.
  uint32_t writeAddr = dma_channel_hw_addr((uint)spiDmaCh)->write_addr;
  uint32_t writeIdx =
      (writeAddr - reinterpret_cast<uint32_t>(spiRingBuf)) / sizeof(uint32_t);
  writeIdx &= (SPI_RING_BUF_WORDS - 1u);

  if (!gInst) {
    // No LcdTap instance to feed; just advance the read index to "consume"
    // the data.
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
  //    Do this first, before the system clock changes, while the default
  //    125 MHz clock is running and the GPIO input synchronisers are stable.
  // -------------------------------------------------------------------------
  for (uint pin : {PIN_CFG_LCD_SIZE, PIN_CFG_DVI_RES, PIN_CFG_ROT0,
                   PIN_CFG_ROT1, PIN_CFG_INV_POL}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_down(pin);
  }
  sleep_ms(1);  // allow pull-downs to settle

  const bool lcd320 = gpio_get(PIN_CFG_LCD_SIZE);
  const bool dvi720p = gpio_get(PIN_CFG_DVI_RES);
  const bool invPolarity = gpio_get(PIN_CFG_INV_POL);
  const int rot =
      static_cast<int>((gpio_get(PIN_CFG_ROT1) << 1u) | gpio_get(PIN_CFG_ROT0));

  const uint16_t lcdW = 240u;
  const uint16_t lcdH = lcd320 ? 320u : 240u;

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
  // 4. RESX input (pull-up; active low from SPI master)
  // -------------------------------------------------------------------------
  gpio_init(PIN_PAR_RESX);
  gpio_set_dir(PIN_PAR_RESX, GPIO_IN);
  gpio_pull_up(PIN_PAR_RESX);

  // -------------------------------------------------------------------------
  // 5. DVI init (claims DMA channels and PIO0 state machines)
  // -------------------------------------------------------------------------
  dvi0.timing = timing;
  dvi0.ser_cfg = pico_sock_cfg;  // GPIO12-19, pio0, invert_diffpairs=false
  dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

  // Effective colour buffer dimensions for dvi_scanbuf_main_16bpp:
  //   Horizontal: tmds_encode_data_channel_16bpp doubles each pixel, so the
  //               colour buffer is h_active_pixels/2 pixels wide.
  //   Vertical:   DVI_VERTICAL_REPEAT reuses each TMDS buffer for REPEAT
  //               consecutive physical lines, so only v_active_lines/REPEAT
  //               unique colour buffers are consumed per DVI frame.
  const uint32_t dviW = timing->h_active_pixels / 2;
  const uint32_t dviH = timing->v_active_lines / DVI_VERTICAL_REPEAT;

  for (int i = 0; i < N_SCANLINE_BUFS; ++i) {
    scanlineBufs[i] =
        static_cast<uint16_t *>(poolAlloc(dviW * sizeof(uint16_t)));
    if (!scanlineBufs[i]) panic("scanline buf alloc failed");
    queue_add_blocking_u32(&dvi0.q_colour_free, &scanlineBufs[i]);
  }

  // -------------------------------------------------------------------------
  // 6. LcdTap init
  // -------------------------------------------------------------------------
  lcdtap::LcdTapConfig cfg;
  lcdtap::getDefaultConfig(lcdtap::ControllerType::ST7789, &cfg);
  cfg.lcdHeight = lcdH;  // 240 or 320 selected by PIN_CFG_LCD_SIZE
  cfg.scaleMode = lcdtap::ScaleMode::FIT;
  cfg.invertInvPolarity = invPolarity;

  // Use effective (colour-buffer) dimensions, not physical DVI dimensions.
  cfg.dviWidth = static_cast<uint16_t>(dviW);
  cfg.dviHeight = static_cast<uint16_t>(dviH);

  lcdtap::HostInterface host;
  host.alloc = poolAlloc;
  host.free = [](void *) {};
  host.log = nullptr;
  host.userData = nullptr;

  lcdtap::LcdTap inst(cfg, host);
  if (inst.getStatus() != lcdtap::Status::OK) panic("LcdTap init failed");

  // Fill framebuffer with 8-bar test pattern so the DVI output pipeline can
  // be verified before the SPI master sends SLPOUT + DISPON.
  // The pattern is replaced by real SPI content once the master initialises.
  // Bar order (left→right): black blue green cyan red magenta yellow white
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

  gInst = &inst;  // expose to IRQ handler

  inst.setOutputRotation(rot);  // apply boot-time rotation setting

  // -------------------------------------------------------------------------
  // 7. RESX interrupt
  // -------------------------------------------------------------------------
  gpio_set_irq_enabled_with_callback(PIN_PAR_RESX,
                                     GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                     /*enabled=*/true, &gpioIrqHandler);

  // -------------------------------------------------------------------------
  // 8. Parallel slave PIO + DMA
  //    Must come after dvi_init() so DVI claims its DMA channels first.
  // -------------------------------------------------------------------------
  uint pioProgOffset = pio_add_program(SPI_PIO, &par_slave_with_dcx_program);
  parSlaveInit(pioProgOffset);
  spiDmaInit();

  // -------------------------------------------------------------------------
  // 9. Launch Core 1 for DVI TMDS output
  // -------------------------------------------------------------------------
  multicore_launch_core1(core1Main);

  // -------------------------------------------------------------------------
  // 10. Main loop (Core 0)
  //     processSpiRingBuf() is called at three points per iteration:
  //       (a) inside the q_colour_free spin — drains while waiting
  //       (b) just before getScanline — drains during scanline preparation
  //       (c) after queue_add_blocking_u32 — drains after handing off
  //     This prevents the ring buffer from overrunning.
  // -------------------------------------------------------------------------
  uint32_t scanY = 0;
  uint32_t frame = 0;
  bool led = false;
  int currentRot = rot;

  while (true) {
    uint16_t *buf;
    while (!queue_try_remove_u32(&dvi0.q_colour_free, &buf)) {
      processSpiRingBuf();  // (a) drain while waiting for free buffer
    }

    // Fill the DVI buffer directly — no intermediate copy needed.
    gInst->fillScanline(static_cast<uint16_t>(scanY), buf);

    // Hand the filled scanline to DVI Core 1.
    queue_add_blocking_u32(&dvi0.q_colour_valid, &buf);

    // Advance scanline counter; detect frame boundary.
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
