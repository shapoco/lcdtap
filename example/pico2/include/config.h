#pragma once

#include <cstdint>

// =============================================================================
// SPI slave pins (PIO1 SM0)
// =============================================================================
static constexpr uint PIN_SPI_SCK  = 2u;  // SPI clock (input)
static constexpr uint PIN_SPI_MOSI = 3u;  // SPI data  (input, PIO IN_BASE)
static constexpr uint PIN_SPI_DCX  = 4u;  // D/CX      (input, PIO JMP_PIN)
static constexpr uint PIN_SPI_CS   = 5u;  // Chip select, active low (input)
static constexpr uint PIN_SPI_RESX = 6u;  // Hardware reset, active low (input)

// =============================================================================
// Boot-time configuration GPIOs (read once at startup)
// Pull LOW = default, pull HIGH = alternate
// =============================================================================
static constexpr uint PIN_CFG_LCD_SIZE    = 20u; // LOW=240x240 / HIGH=240x320
static constexpr uint PIN_CFG_DVI_RES     = 21u; // LOW=640x480@60Hz / HIGH=1280x720@30Hz(reduced)
static constexpr uint PIN_CFG_SCALE_MODE0 = 10u; // \__ scale mode bits
static constexpr uint PIN_CFG_SCALE_MODE1 = 11u; // /   00=STRETCH 01=FIT 10=PIXEL_PERFECT

// =============================================================================
// Onboard LED
// =============================================================================
static constexpr uint PIN_LED = 25u;

// =============================================================================
// Debug probe GPIO (optional — connect to oscilloscope / logic analyser)
//   PIN_DBG_FRAME: pulses once per DVI frame (scan_y == 0).
//                 Expected frequency: ~60 Hz (640x480) or ~30 Hz (1280x720).
//                 Seeing this signal confirms the DVI main loop is running.
// =============================================================================
static constexpr uint PIN_DBG_FRAME = 7u;

// =============================================================================
// PIO / DMA resource assignment
// pio0 is reserved for PicoDVI (libdvi uses DVI_DEFAULT_PIO_INST = pio0).
// pio1 is used for the SPI slave.
// DMA channels are claimed dynamically; DVI claims its channels inside
// dvi_init(), so spi_dma_init() must be called after dvi_init().
// =============================================================================
#define SPI_PIO  pio1
static constexpr uint SPI_SM = 0u;

// =============================================================================
// SPI DMA ring buffer
// Must be a power-of-two number of bytes and aligned to its own size.
// Each element is one uint32_t word: bit[8]=DCX, bits[7:0]=data byte.
// =============================================================================
static constexpr uint32_t SPI_RING_BUF_BYTES    = 4096u;
static constexpr uint32_t SPI_RING_BUF_WORDS    = SPI_RING_BUF_BYTES / sizeof(uint32_t);
static constexpr uint32_t SPI_RING_BUF_LOG2     = 12u; // log2(4096)

// =============================================================================
// DVI scanline buffers (RGB565, fed to PicoDVI q_colour_valid)
// Must be <= q_colour_free queue depth (8, set in dvi_init).
// =============================================================================
static constexpr int N_SCANLINE_BUFS = 4;

// =============================================================================
// LED blink interval (DVI output frames)
// =============================================================================
static constexpr uint32_t LED_TOGGLE_FRAMES = 30u;

// =============================================================================
// Memory pool for SpiLcd2Dvi internal allocations (bump allocator)
// 240x320 RGB565 framebuffer = 153 600 bytes
// + scanline buf (320x2 = 640) + DVI bufs (4x320x2 = 2560) + Impl (~256)
// =============================================================================
static constexpr size_t MEM_POOL_SIZE = 200u * 1024u;

// =============================================================================
// Data batching buffer for inputData() calls
// =============================================================================
static constexpr size_t DATA_BATCH_CAP = 256u;
