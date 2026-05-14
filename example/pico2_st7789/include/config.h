#pragma once

#include <cstdint>

// =============================================================================
// Parallel slave pins (PIO1 SM0)
//
// External circuit: 74HC595 (serial-in / parallel-out) + 74HC4040 (÷8) + 74AHC1G04 (inverter)
//   SPI SCLK  → 74HC4040 CP, 74HC595 SRCLK
//   SPI MOSI  → 74HC595 SER
//   SPI CS    → 74HC4040 CLR  (active-high; holds Q3=0, BCLK=1 while CS de-asserted)
//   74HC4040 Q3 → 74AHC1G04 → 74HC595 RCLK, GPIO 2  (BCLK = SCLK/8, HIGH when byte complete)
//   74HC595 Q1 → GPIO 4  (D[0], LSB)  ... Q8 → GPIO 11 (D[7], MSB)
//   DCX       → GPIO 3   (D/C# signal, direct from SPI master)
//   RESX      → GPIO 22  (hardware reset, active low)
// =============================================================================
// SCLK/8 byte clock (input)
static constexpr uint PIN_PAR_BCLK = 2u;

// D/C# (input, PIO JMP_PIN)
static constexpr uint PIN_PAR_DCX = 3u;

// D[0..7] (input, PIO IN_BASE; GPIO 4-11)
static constexpr uint PIN_PAR_DATA_BASE = 4u;

// Hardware reset, active low (input)
static constexpr uint PIN_PAR_RESX = 22u;

// =============================================================================
// Boot-time configuration GPIOs (read once at startup)
// Pull LOW = default, pull HIGH = alternate
// =============================================================================

// LOW=240x240 / HIGH=240x320
static constexpr uint PIN_CFG_LCD_SIZE = 20u;

// LOW=640x480@60Hz / HIGH=1280x720@30Hz(reduced)
static constexpr uint PIN_CFG_DVI_RES = 21u;

// output rotation bits: GPIO26=rot bit0, GPIO27=rot bit1 (pull-down=0, default rot=0)
static constexpr uint PIN_CFG_ROT0 = 26u;
static constexpr uint PIN_CFG_ROT1 = 27u;

// =============================================================================
// Onboard LED
// =============================================================================
static constexpr uint PIN_LED = 25u;

// =============================================================================
// Boot-time inversion polarity GPIO (read once at startup)
// Pull LOW = default (INVON→inverted), pull HIGH = polarity inverted (INVON→non-inverted)
// =============================================================================
static constexpr uint PIN_CFG_INV_POL = 28u;

// =============================================================================
// PIO / DMA resource assignment
// pio0 is reserved for PicoDVI (libdvi uses DVI_DEFAULT_PIO_INST = pio0).
// pio1 is used for the parallel slave.
// DMA channels are claimed dynamically; DVI claims its channels inside
// dvi_init(), so spiDmaInit() must be called after dvi_init().
// =============================================================================
#define SPI_PIO pio1
static constexpr uint SPI_SM = 0u;

// =============================================================================
// SPI ring buffer
// Must be a power-of-two number of bytes and aligned to its own size.
// Each element is one uint32_t word: bit[8]=DCX, bits[7:0]=data byte.
// =============================================================================
static constexpr uint32_t SPI_RING_BUF_LOG2 = 14u;
static constexpr uint32_t SPI_RING_BUF_BYTES = 1u << SPI_RING_BUF_LOG2;
static constexpr uint32_t SPI_RING_BUF_WORDS =
    SPI_RING_BUF_BYTES / sizeof(uint32_t);

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
// Memory pool for LcdTap internal allocations (bump allocator)
// 240x320 RGB565 framebuffer = 153 600 bytes
// + scanline buf (320x2 = 640) + DVI bufs (4x320x2 = 2560) + Impl (~256)
// =============================================================================
static constexpr size_t MEM_POOL_SIZE = 200u * 1024u;

// =============================================================================
// Data batching buffer for inputData() calls
// =============================================================================
static constexpr size_t DATA_BATCH_CAP = 1024u;
