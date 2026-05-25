#pragma once

#include <cstdint>
#include <dvi.h>

// =============================================================================
// Common pins (all interface modes)
// =============================================================================

// Hardware reset, active low (input, pull-up)
static constexpr uint PIN_RST = 0u;

// =============================================================================
// 4-Line SPI slave pins (spi_4line_mode0.pio, PIO1 SM0)
//
// Direct connection — no external ICs required.
//   RST  → GPIO 0  (hardware reset, active low, pull-up on Pico 2)
//   CS   → GPIO 1  (chip select, active low; must match SPI_CS_PIN in
//                   spi_4line_mode0.pio)
//   SCLK → GPIO 2  (clock, CPOL=0: idle LOW; must match SPI_SCLK_PIN in
//                   spi_4line_mode0.pio)
//   MOSI → GPIO 3  (data MSB first; PIO IN_BASE)
//   DC   → GPIO 4  (D/C# signal; sampled as IN_BASE+1 in spi_4line_mode0.pio)
// =============================================================================

// Chip select, active low (input; must equal SPI_CS_PIN defined in
// spi_4line_mode0.pio = 1)
static constexpr uint PIN_SPI_CS = 1u;

// SPI clock (input; must equal SPI_SCLK_PIN defined in spi_4line_mode0.pio = 2)
static constexpr uint PIN_SPI_SCLK = 2u;

// SPI data (input, PIO IN_BASE)
static constexpr uint PIN_SPI_MOSI = 3u;

// D/C# signal (input, PIO IN_BASE+1; 4-line SPI only)
static constexpr uint PIN_SPI_DC = 4u;

// =============================================================================
// Parallel slave pins (parallel_8bit.pio, PIO1 SM0)
//
// Direct connection — no external ICs required.
//   RST  → GPIO 0   (hardware reset, shared with SPI)
//   CS   → GPIO 1   (chip select, active low; shared with PIN_SPI_CS)
//   WR#  → GPIO 2   (write strobe, active low; must match PAR_WR_PIN)
//   D[0] → GPIO 3   (D[0], PIO IN_BASE)  ... D[7] → GPIO 10
//   DC   → GPIO 11  (D/C# signal; sampled as IN_BASE+8)
// =============================================================================

// Chip select, active low (shared with PIN_SPI_CS; must match PAR_CS_PIN)
static constexpr uint PIN_PAR_CS = 1u;

// Write strobe, active low (must match PAR_WR_PIN defined in parallel_8bit.pio)
static constexpr uint PIN_PAR_WR = 2u;

// D[0..7] (input, PIO IN_BASE; GPIO 3-10)
static constexpr uint PIN_PAR_DATA_BASE = 3u;

// D/C# signal (input, PIO IN_BASE+8 = GPIO 11)
static constexpr uint PIN_PAR_DC = 11u;

// =============================================================================
// I2C slave pins (I2C0, software slave mode)
//
//   SDA → GPIO 8
//   SCL → GPIO 9
// =============================================================================
static constexpr uint PIN_I2C_SDA = 8u;
static constexpr uint PIN_I2C_SCL = 9u;
static constexpr uint I2C_SLAVE_ADDR = 0x3Cu;

// =============================================================================
// Boot-time configuration GPIOs (active-low, internal pull-up)
// Default (not connected, pull-up HIGH) = primary mode.
// Driven LOW = alternate mode.
// =============================================================================

// LOW=1280×720@30Hz / HIGH=640×480@60Hz (default)
static constexpr uint PIN_CFG_OUT_720P = 20u;

// =============================================================================
// Key inputs (active-low, internal pull-up)
// =============================================================================
static constexpr uint PIN_KEY_UP = 26u;
static constexpr uint PIN_KEY_DOWN = 21u;
static constexpr uint PIN_KEY_LEFT = 22u;
static constexpr uint PIN_KEY_RIGHT = 27u;
static constexpr uint PIN_KEY_ENTER = 28u;

// =============================================================================
// Onboard LED
// =============================================================================
static constexpr uint PIN_LED = 25u;

// =============================================================================
// PIO / DMA resource assignment
// pio0 is reserved for PicoDVI (libdvi uses DVI_DEFAULT_PIO_INST = pio0).
// pio1 is used for SPI/Parallel slave interfaces.
// =============================================================================
#define SPI_PIO pio1
static constexpr uint SPI_SM = 0u;

// =============================================================================
// SPI ring buffer
// Must be a power-of-two number of bytes and aligned to its own size.
// Each element is one uint32_t word: bit[8]=DC, bits[7:0]=data byte.
// =============================================================================
static constexpr uint32_t SPI_RING_BUF_LOG2 = 14u;
static constexpr uint32_t SPI_RING_BUF_BYTES = 1u << SPI_RING_BUF_LOG2;
static constexpr uint32_t SPI_RING_BUF_WORDS =
    SPI_RING_BUF_BYTES / sizeof(uint32_t);

// =============================================================================
// I2C ring buffer
// Filled by IRQ handler; drained by main loop.
// Same word format: bit[8]=DC, bits[7:0]=data byte.
// =============================================================================
static constexpr uint32_t I2C_RING_BUF_WORDS = 256u;  // 1 KB

// =============================================================================
// DVI scanline buffers (RGB565, fed to PicoDVI q_colour_valid)
// Must be <= q_colour_free queue depth (8, set in dvi_init).
// DVI_MAX_W is the maximum effective width (h_active_pixels/2) across all
// supported DVI timings. Used to statically size the scanline buffers.
// =============================================================================
static constexpr int N_SCANLINE_BUFS = 4;
static constexpr uint32_t DVI_MAX_W = 1280 / DVI_SYMBOLS_PER_WORD;

// =============================================================================
// LED blink interval (DVI output frames)
// =============================================================================
static constexpr uint32_t LED_TOGGLE_FRAMES = 30u;

// =============================================================================
// Memory pool for the LcdTap framebuffer (sole user of the pool).
// Scanline buffers are now statically allocated (see DVI_MAX_W above).
// Sized to fit the largest practical framebuffer: 320x480 RGB565 = 307 200 B.
// =============================================================================
static constexpr size_t MEM_POOL_SIZE = 310u * 1024u;

// =============================================================================
// Framebuffer size defaults (overridable via cmake -DLCDTAP_LCD_SIZE_W= etc.)
// Adjustable at runtime via the OSD menu.
// =============================================================================
#ifndef LCDTAP_LCD_SIZE_W
#define LCDTAP_LCD_SIZE_W 240
#endif
#ifndef LCDTAP_LCD_SIZE_H
#define LCDTAP_LCD_SIZE_H 320
#endif
