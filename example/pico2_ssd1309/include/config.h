#pragma once

#include <cstdint>

// =============================================================================
// Parallel slave pins (SPI mode, PIO1 SM0)
//
// External circuit: 74HC4094 (serial-in / parallel-out) + 74HC4040 (÷8)
//   SPI SCLK  → 74HC4040 CP, 74HC4094 CP
//   SPI MOSI  → 74HC4094 DS
//   SPI CS    → 74HC4040 CLR  (active-high; holds BCLK=0 while CS de-asserted)
//   74HC4040 Q3 → GPIO 2  (BCLK = SCLK/8)
//   74HC4094 Q1 → GPIO 4  (D[0], LSB)  ... Q8 → GPIO 11 (D[7], MSB)
//   DCX       → GPIO 3   (D/C# signal, direct from SPI master)
//   RESX      → GPIO 22  (hardware reset, active low)
// =============================================================================
// SCLK/8 byte clock (input)
static constexpr uint PIN_PAR_BCLK = 2u;

// D/C# (input, PIO JMP_PIN)
static constexpr uint PIN_PAR_DCX = 3u;

// D[0..7] (input, PIO IN_BASE; GPIO 4-11)
static constexpr uint PIN_PAR_DATA_BASE = 4u;

// Hardware reset, active low (input, SPI mode)
static constexpr uint PIN_PAR_RESX = 22u;

// =============================================================================
// I2C slave pins (I2C mode, I2C0)
//
// SSD1309 I2C address: 0x3C (SA0 tied low)
// Connect SSD1309 SDA/SCL directly to Pico 2; add 4.7kΩ pull-ups to 3.3V.
// =============================================================================
static constexpr uint PIN_I2C_SDA = 0u;
static constexpr uint PIN_I2C_SCL = 1u;
static constexpr uint I2C_SLAVE_ADDR = 0x3Cu;

// =============================================================================
// Boot-time configuration GPIOs (read once at startup)
// Pull LOW = default, pull HIGH = alternate
// =============================================================================

// LOW=I2C mode (default) / HIGH=SPI mode
static constexpr uint PIN_CFG_INPUT_MODE = 20u;

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
// PIO / DMA resource assignment (SPI mode)
// pio0 is reserved for PicoDVI (libdvi uses DVI_DEFAULT_PIO_INST = pio0).
// pio1 is used for the parallel slave.
// =============================================================================
#define SPI_PIO pio1
static constexpr uint SPI_SM = 0u;

// =============================================================================
// SPI ring buffer (SPI mode)
// Must be a power-of-two number of bytes and aligned to its own size.
// Each element is one uint32_t word: bit[8]=DCX, bits[7:0]=data byte.
// =============================================================================
static constexpr uint32_t SPI_RING_BUF_LOG2 = 12u;  // 4KB (SSD1309 is small)
static constexpr uint32_t SPI_RING_BUF_BYTES = 1u << SPI_RING_BUF_LOG2;
static constexpr uint32_t SPI_RING_BUF_WORDS =
    SPI_RING_BUF_BYTES / sizeof(uint32_t);

// =============================================================================
// I2C ring buffer (I2C mode)
// Filled by IRQ handler; drained by main loop.
// Same word format as SPI ring buffer: bit[8]=DCX, bits[7:0]=data byte.
// =============================================================================
static constexpr uint32_t I2C_RING_BUF_WORDS = 256u;  // 1KB

// =============================================================================
// DVI scanline buffers (RGB565, fed to PicoDVI q_colour_valid)
// =============================================================================
static constexpr int N_SCANLINE_BUFS = 4;

// =============================================================================
// LED blink interval (DVI output frames)
// =============================================================================
static constexpr uint32_t LED_TOGGLE_FRAMES = 30u;

// =============================================================================
// Memory pool for LcdTap internal allocations (bump allocator)
// SSD1309 framebuffer: 128x64x2 = 16 384 bytes
// + scanline bufs (320x2x4 = 2 560) + impl (~256)
// =============================================================================
static constexpr size_t MEM_POOL_SIZE = 32u * 1024u;

// =============================================================================
// Data batching buffer for inputData() calls (SPI mode)
// =============================================================================
static constexpr size_t DATA_BATCH_CAP = 256u;
