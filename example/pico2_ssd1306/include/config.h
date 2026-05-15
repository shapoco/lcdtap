#pragma once

#include <cstdint>

// =============================================================================
// SPI slave pins (SPI mode, PIO1 SM0)
//
// Direct connection — no external ICs required.
//   SPI RESX → GPIO 1  (hardware reset, active low, pull-up on Pico 2)
//   SPI SCLK → GPIO 2  (clock, CPOL=0: idle LOW; must match SPI_SCLK_PIN in
//   spi_slave.pio) SPI MOSI → GPIO 4  (data MSB first; PIO IN_BASE) SPI DCX  →
//   GPIO 5  (D/C# signal; sampled as IN_BASE+1 in spi_slave.pio) SPI CS   →
//   GPIO 6  (chip select, active low; must match SPI_CS_PIN in spi_slave.pio)
// =============================================================================
// Hardware reset, active low (input, pull-up)
static constexpr uint PIN_SPI_RESX = 1u;

// SPI clock (input; must equal SPI_SCLK_PIN defined in spi_slave.pio = 2)
static constexpr uint PIN_SPI_SCLK = 2u;

// SPI data (input, PIO IN_BASE)
static constexpr uint PIN_SPI_MOSI = 4u;

// D/C# signal (input, PIO IN_BASE+1)
static constexpr uint PIN_SPI_DCX = 5u;

// Chip select, active low (input; must equal SPI_CS_PIN defined in
// spi_slave.pio = 6)
static constexpr uint PIN_SPI_CS = 6u;

// =============================================================================
// I2C slave pins (I2C mode, I2C0)
//
// SSD1306 I2C address: 0x3C (SA0 tied low)
// Connect SSD1306 SDA/SCL directly to Pico 2; add 4.7kΩ pull-ups to 3.3V.
// =============================================================================
static constexpr uint PIN_I2C_SDA = 8u;
static constexpr uint PIN_I2C_SCL = 9u;
static constexpr uint I2C_SLAVE_ADDR = 0x3Cu;

// =============================================================================
// Boot-time configuration GPIOs (read once at startup)
// Pull LOW = default, pull HIGH = alternate
// =============================================================================

// LOW=I2C mode (default) / HIGH=SPI mode
static constexpr uint PIN_CFG_INPUT_MODE = 20u;

// LOW=640x480@60Hz / HIGH=1280x720@30Hz(reduced)
static constexpr uint PIN_CFG_DVI_RES = 21u;

// output rotation bits: GPIO26=rot bit0, GPIO27=rot bit1 (pull-down=0, default
// rot=0)
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
static constexpr uint32_t SPI_RING_BUF_LOG2 = 12u;  // 4KB (SSD1306 is small)
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
// SSD1306 framebuffer: 128x64x2 = 16 384 bytes
// + scanline bufs (320x2x4 = 2 560) + impl (~256)
// =============================================================================
static constexpr size_t MEM_POOL_SIZE = 32u * 1024u;

// =============================================================================
// Data batching buffer for inputData() calls (SPI mode)
// =============================================================================
static constexpr size_t DATA_BATCH_CAP = 256u;

// =============================================================================
// Framebuffer size defaults (overridable via cmake -DLCDTAP_LCD_SIZE_W=...
// etc.)
// =============================================================================
#ifndef LCDTAP_LCD_SIZE_W
#define LCDTAP_LCD_SIZE_W 128
#endif
#ifndef LCDTAP_LCD_SIZE_H
#define LCDTAP_LCD_SIZE_H 64
#endif
