#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"

#include "dvi.h"
#include "dvi_timing.h"
#include "common_dvi_pin_configs.h"   // pico_sock_cfg

#include "spilcd2dvi/spilcd2dvi.hpp"

#include "config.h"
#include "spi_slave.pio.h"

// =============================================================================
// Memory pool (bump allocator for SpiLcd2Dvi)
// =============================================================================
static uint8_t mem_pool[MEM_POOL_SIZE];
static size_t  mem_pool_offset = 0;

static void *pool_alloc(size_t size) {
    // 4-byte align
    size = (size + 3u) & ~3u;
    if (mem_pool_offset + size > sizeof(mem_pool))
        return nullptr;
    void *p = mem_pool + mem_pool_offset;
    mem_pool_offset += size;
    return p;
}

// =============================================================================
// SPI ring buffer  (word = [bit8: DCX, bits7:0: data byte])
// =============================================================================
static uint32_t __attribute__((aligned(SPI_RING_BUF_BYTES)))
    spi_ring_buf[SPI_RING_BUF_WORDS];

static int      spi_dma_ch   = -1;
static uint32_t spi_read_idx = 0;  // index into spi_ring_buf[]

// =============================================================================
// DVI
// =============================================================================
static struct dvi_inst dvi0;

// Pre-allocated RGB565 scanline buffers, recycled via q_colour_free.
static uint16_t *scanline_bufs[N_SCANLINE_BUFS];

// =============================================================================
// SpiLcd2Dvi instance (set after init so the RESX callback can reach it)
// =============================================================================
static sl2d::SpiLcd2Dvi *g_sl2d = nullptr;

// =============================================================================
// GPIO interrupt handler  (RESX pin)
// =============================================================================
static void gpio_irq_handler(uint gpio, uint32_t events) {
    if (gpio == PIN_SPI_RESX && g_sl2d) {
        g_sl2d->inputReset((events & GPIO_IRQ_EDGE_FALL) != 0u);
    }
}

// =============================================================================
// Core 1: DVI TMDS encode + serialise (never returns)
// =============================================================================
static void core1_main() {
    // Route DVI DMA IRQ to this core; use DMA_IRQ_0 (highest priority).
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);

    // Wait until Core 0 has queued the first scanline before starting output.
    while (queue_is_empty(&dvi0.q_colour_valid))
        __wfe();

    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);  // infinite loop
}

// =============================================================================
// SPI slave init  (PIO1 SM0)
// =============================================================================
static void spi_slave_init(uint prog_offset) {
    // All SPI pins are inputs.
    gpio_init(PIN_SPI_SCK);  gpio_set_dir(PIN_SPI_SCK,  GPIO_IN);
    gpio_init(PIN_SPI_MOSI); gpio_set_dir(PIN_SPI_MOSI, GPIO_IN);
    gpio_init(PIN_SPI_DCX);  gpio_set_dir(PIN_SPI_DCX,  GPIO_IN);
    gpio_init(PIN_SPI_CS);   gpio_set_dir(PIN_SPI_CS,   GPIO_IN);
    // CS and RESX pulled high so the line reads "deasserted" when unconnected.
    gpio_pull_up(PIN_SPI_CS);
    gpio_pull_up(PIN_SPI_RESX);

    pio_sm_config c = spi_slave_with_dcx_program_get_default_config(prog_offset);
    // MOSI is the IN base pin; 'in pins, 1' samples from here.
    sm_config_set_in_pins(&c, PIN_SPI_MOSI);
    // DCX is the JMP_PIN; 'jmp pin' branches on its state.
    sm_config_set_jmp_pin(&c, PIN_SPI_DCX);
    // Shift left so MSB-first bits accumulate in [7:0] after 8 shifts.
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
// SPI DMA init  (call AFTER dvi_init so DVI has already claimed its channels)
// =============================================================================
static void spi_dma_init() {
    spi_dma_ch = dma_claim_unused_channel(true);

    dma_channel_config cfg = dma_channel_get_default_config((uint)spi_dma_ch);
    channel_config_set_read_increment(&cfg, false);   // fixed: PIO RX FIFO
    channel_config_set_write_increment(&cfg, true);   // advances through ring
    channel_config_set_dreq(&cfg, pio_get_dreq(SPI_PIO, SPI_SM, /*is_tx=*/false));
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    // Wrap the write address at SPI_RING_BUF_BYTES boundary → circular buffer.
    channel_config_set_ring(&cfg, /*write=*/true, SPI_RING_BUF_LOG2);

    dma_channel_configure(
        (uint)spi_dma_ch, &cfg,
        spi_ring_buf,               // initial write address
        &SPI_PIO->rxf[SPI_SM],      // read address: PIO RX FIFO
        0xFFFFFFFFu,                // run forever
        /*trigger=*/true);
}

// =============================================================================
// Drain the SPI ring buffer and feed bytes to SpiLcd2Dvi
// =============================================================================
static uint8_t data_batch[DATA_BATCH_CAP];
static size_t  data_batch_len = 0;

static void flush_data_batch() {
    if (data_batch_len > 0 && g_sl2d) {
        g_sl2d->inputData(data_batch, data_batch_len);
        data_batch_len = 0;
    }
}

static void process_spi_ring_buf() {
    // Current write position from the DMA controller.
    uint32_t write_addr =
        dma_channel_hw_addr((uint)spi_dma_ch)->write_addr;
    uint32_t write_idx =
        (write_addr - reinterpret_cast<uint32_t>(spi_ring_buf))
        / sizeof(uint32_t);
    // Mask to ring size (the ring wrapping keeps the address in range,
    // but recalculate the index defensively).
    write_idx &= (SPI_RING_BUF_WORDS - 1u);

    while (spi_read_idx != write_idx) {
        uint32_t word = spi_ring_buf[spi_read_idx];
        spi_read_idx  = (spi_read_idx + 1u) & (SPI_RING_BUF_WORDS - 1u);

        const bool  is_data = (word >> 8u) & 1u;
        const uint8_t byte  = static_cast<uint8_t>(word);

        if (is_data) {
            data_batch[data_batch_len++] = byte;
            if (data_batch_len >= DATA_BATCH_CAP)
                flush_data_batch();
        } else {
            flush_data_batch();
            if (g_sl2d)
                g_sl2d->inputCommand(byte);
        }
    }

    // Flush any partial data run at the end of the available bytes.
    flush_data_batch();
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
    for (uint pin : {PIN_CFG_LCD_SIZE, PIN_CFG_DVI_RES,
                     PIN_CFG_SCALE_MODE0, PIN_CFG_SCALE_MODE1}) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_down(pin);
    }
    sleep_ms(1);  // allow pull-downs to settle

    const bool lcd_320   = gpio_get(PIN_CFG_LCD_SIZE);
    const bool dvi_720p  = gpio_get(PIN_CFG_DVI_RES);
    const uint8_t scale  =
        static_cast<uint8_t>(
            (gpio_get(PIN_CFG_SCALE_MODE1) << 1u) |
             gpio_get(PIN_CFG_SCALE_MODE0));

    const uint16_t lcd_w = 240u;
    const uint16_t lcd_h = lcd_320 ? 320u : 240u;

    const struct dvi_timing *timing = dvi_720p
        ? &dvi_timing_1280x720p_reduced_30hz   // 319.2 MHz bit clock
        : &dvi_timing_640x480p_60hz;            // 252.0 MHz bit clock

    sl2d::ScaleMode scale_mode;
    switch (scale & 0x3u) {
        case 1u: scale_mode = sl2d::ScaleMode::FIT;           break;
        case 2u: scale_mode = sl2d::ScaleMode::PIXEL_PERFECT; break;
        default: scale_mode = sl2d::ScaleMode::STRETCH;       break;
    }

    // -------------------------------------------------------------------------
    // 2. Voltage regulator and system clock
    // -------------------------------------------------------------------------
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(timing->bit_clk_khz, /*required=*/true);

    // -------------------------------------------------------------------------
    // 3. LED + debug probe
    // -------------------------------------------------------------------------
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);

    gpio_init(PIN_DBG_FRAME);
    gpio_set_dir(PIN_DBG_FRAME, GPIO_OUT);
    gpio_put(PIN_DBG_FRAME, 0);

    // -------------------------------------------------------------------------
    // 4. RESX input (pull-up; active low from SPI master)
    // -------------------------------------------------------------------------
    gpio_init(PIN_SPI_RESX);
    gpio_set_dir(PIN_SPI_RESX, GPIO_IN);
    gpio_pull_up(PIN_SPI_RESX);

    // -------------------------------------------------------------------------
    // 5. DVI init (claims DMA channels and PIO0 state machines)
    // -------------------------------------------------------------------------
    dvi0.timing  = timing;
    dvi0.ser_cfg = pico_sock_cfg;  // GPIO12-19, pio0, invert_diffpairs=false
    dvi_init(&dvi0,
             next_striped_spin_lock_num(),
             next_striped_spin_lock_num());

    // Effective colour buffer dimensions for dvi_scanbuf_main_16bpp:
    //   Horizontal: tmds_encode_data_channel_16bpp doubles each pixel, so the
    //               colour buffer is h_active_pixels/2 pixels wide.
    //   Vertical:   DVI_VERTICAL_REPEAT reuses each TMDS buffer for REPEAT
    //               consecutive physical lines, so only v_active_lines/REPEAT
    //               unique colour buffers are consumed per DVI frame.
    const uint32_t dvi_w = timing->h_active_pixels / 2;
    const uint32_t dvi_h = timing->v_active_lines / DVI_VERTICAL_REPEAT;

    for (int i = 0; i < N_SCANLINE_BUFS; ++i) {
        scanline_bufs[i] = static_cast<uint16_t *>(
            pool_alloc(dvi_w * sizeof(uint16_t)));
        if (!scanline_bufs[i])
            panic("scanline buf alloc failed");
        queue_add_blocking_u32(&dvi0.q_colour_free, &scanline_bufs[i]);
    }

    // -------------------------------------------------------------------------
    // 6. SpiLcd2Dvi init
    // -------------------------------------------------------------------------
    sl2d::Sl2dConfig sl2d_cfg;
    sl2d_cfg.lcdWidth   = lcd_w;
    sl2d_cfg.lcdHeight  = lcd_h;
    sl2d_cfg.pixelFormat = sl2d::PixelFormat::RGB565;
    sl2d_cfg.scaleMode  = scale_mode;

    // Pixel clock = TMDS bit clock / 10
    sl2d_cfg.dviTiming.pixelClockKhz = timing->bit_clk_khz / 10u;

    // Use effective (colour-buffer) dimensions, not physical DVI dimensions.
    sl2d_cfg.dviTiming.h.active      = static_cast<uint16_t>(dvi_w);
    sl2d_cfg.dviTiming.h.frontPorch  = static_cast<uint16_t>(timing->h_front_porch);
    sl2d_cfg.dviTiming.h.syncWidth   = static_cast<uint16_t>(timing->h_sync_width);
    sl2d_cfg.dviTiming.h.backPorch   = static_cast<uint16_t>(timing->h_back_porch);
    sl2d_cfg.dviTiming.h.syncPolarity = timing->h_sync_polarity;

    sl2d_cfg.dviTiming.v.active      = static_cast<uint16_t>(dvi_h);
    sl2d_cfg.dviTiming.v.frontPorch  = static_cast<uint16_t>(timing->v_front_porch);
    sl2d_cfg.dviTiming.v.syncWidth   = static_cast<uint16_t>(timing->v_sync_width);
    sl2d_cfg.dviTiming.v.backPorch   = static_cast<uint16_t>(timing->v_back_porch);
    sl2d_cfg.dviTiming.v.syncPolarity = timing->v_sync_polarity;

    sl2d::HostInterface host;
    host.alloc    = pool_alloc;
    host.free     = [](void *) {};
    host.log      = nullptr;
    host.userData = nullptr;

    sl2d::SpiLcd2Dvi sl2d_inst(sl2d_cfg, host);
    if (sl2d_inst.getStatus() != sl2d::Status::OK)
        panic("SpiLcd2Dvi init failed");

    // Fill framebuffer with 8-bar test pattern so the DVI output pipeline can
    // be verified before the SPI master sends SLPOUT + DISPON.
    // The pattern is replaced by real SPI content once the master initialises.
    // Bar order (left→right): black blue green cyan red magenta yellow white
    {
        static const uint16_t kBars[8] = {
            0x0000u, 0x001Fu, 0x07E0u, 0x07FFu,
            0xF800u, 0xF81Fu, 0xFFE0u, 0xFFFFu,
        };
        uint16_t* fb = sl2d_inst.getFramebuf();
        for (uint16_t y = 0; y < lcd_h; ++y) {
            for (uint16_t x = 0; x < lcd_w; ++x) {
                fb[(uint32_t)y * lcd_w + x] =
                    kBars[(uint32_t)x * 8u / lcd_w];
            }
        }
        sl2d_inst.setDisplayOn(true);
    }

    g_sl2d = &sl2d_inst;  // expose to IRQ handler

    // -------------------------------------------------------------------------
    // 7. RESX interrupt
    // -------------------------------------------------------------------------
    gpio_set_irq_enabled_with_callback(
        PIN_SPI_RESX,
        GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
        /*enabled=*/true,
        &gpio_irq_handler);

    // -------------------------------------------------------------------------
    // 8. SPI slave PIO + DMA
    //    Must come after dvi_init() so DVI claims its DMA channels first.
    // -------------------------------------------------------------------------
    uint pio_prog_offset = pio_add_program(SPI_PIO, &spi_slave_with_dcx_program);
    spi_slave_init(pio_prog_offset);
    spi_dma_init();

    // -------------------------------------------------------------------------
    // 9. Launch Core 1 for DVI TMDS output
    // -------------------------------------------------------------------------
    multicore_launch_core1(core1_main);

    // -------------------------------------------------------------------------
    // 10. Main loop (Core 0)
    //     SPI processing runs while WAITING for a free DVI buffer so that
    //     it never delays scanline delivery to Core 1.  If process_spi_ring_buf()
    //     were called after queue_add_blocking_u32 instead, heavy SPI traffic
    //     would starve Core 1 of colour buffers and produce a solid-red error
    //     frame from PicoDVI's late-scanline fallback.
    // -------------------------------------------------------------------------
    uint32_t scan_y = 0;
    uint32_t frame  = 0;
    bool     led    = false;

    while (true) {
        // Drain the SPI ring buffer while waiting for a free DVI scanline
        // buffer.  queue_try_remove_u32 returns immediately without blocking,
        // so Core 0 does useful SPI work instead of spinning idle.
        uint16_t *buf;
        while (!queue_try_remove_u32(&dvi0.q_colour_free, &buf)) {
            process_spi_ring_buf();
        }

        // Copy one RGB565 scanline directly into the DVI buffer.
        const uint16_t *rgb565 = g_sl2d->getScanline(static_cast<uint16_t>(scan_y));
        memcpy(buf, rgb565, dvi_w * sizeof(uint16_t));

        // Hand the filled scanline to DVI Core 1.
        queue_add_blocking_u32(&dvi0.q_colour_valid, &buf);

        // Advance scanline counter; detect frame boundary.
        if (++scan_y >= dvi_h) {
            scan_y = 0;
            // Pulse PIN_DBG_FRAME once per frame — measure ~60 Hz on a scope
            // to confirm the DVI output loop is running at the correct rate.
            gpio_put(PIN_DBG_FRAME, 1);
            gpio_put(PIN_DBG_FRAME, 0);
            if (++frame % LED_TOGGLE_FRAMES == 0u) {
                led = !led;
                gpio_put(PIN_LED, led ? 1 : 0);
            }
        }
    }
}
