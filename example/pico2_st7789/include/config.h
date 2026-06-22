#pragma once

#include <cstdint>

// =============================================================================
// SPI slave pins (PIO1 SM0)
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

// Hardware reset, active low (input, pull-up)
static constexpr uint PIN_RST = 0u;

// Chip select, active low (input; must equal SPI_CS_PIN defined in
// spi_4line_mode0.pio = 1)
static constexpr uint PIN_SPI_CS = 1u;

// SPI clock (input; must equal SPI_SCLK_PIN defined in spi_4line_mode0.pio = 2)
static constexpr uint PIN_SPI_SCLK = 2u;

// SPI data (input, PIO IN_BASE)
static constexpr uint PIN_SPI_MOSI = 3u;

// D/C# signal (input, PIO IN_BASE+1)
static constexpr uint PIN_SPI_DC = 4u;

// =============================================================================
// Boot-time configuration GPIOs (active-low, internal pull-up)
// Default (not connected, pull-up HIGH) = primary mode.
// Driven LOW = alternate mode.
// =============================================================================

// LOW=1280×720@30Hz / HIGH=640×480@60Hz (default)
static constexpr uint PIN_CFG_OUT_720P = 20u;

// LOW=Size2 / HIGH=Size1 (default)
static constexpr uint PIN_CFG_LCD_SIZE_SEL = 21u;

// LOW=swap R and B / HIGH=no swap (default)
static constexpr uint PIN_CFG_SWAP_RB = 22u;

// LOW=inverted polarity / HIGH=normal (default)
static constexpr uint PIN_CFG_INVERTED = 26u;

// Output rotation: CFG_ROT[1:0] active-low
//   HIGH,HIGH → rot=0  no rotation (default)
//   HIGH,LOW  → rot=1  90° CW
//   LOW,HIGH  → rot=2  180°
//   LOW,LOW   → rot=3  270° CW
static constexpr uint PIN_CFG_ROT0 = 27u;
static constexpr uint PIN_CFG_ROT1 = 28u;

// =============================================================================
// Onboard LED
// =============================================================================
static constexpr uint PIN_LED = 25u;

// =============================================================================
// PIO / DMA resource assignment
// pio1 is used for the SPI slave.
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
// Data batching buffer for inputData() calls
// =============================================================================
static constexpr size_t DATA_BATCH_CAP = 1024u;

// =============================================================================
// Framebuffer size defaults (overridable via cmake -DLCDTAP_LCD_SIZE1_W= etc.)
// PIN_CFG_LCD_SIZE_SEL=HIGH → Size1 (default)
// PIN_CFG_LCD_SIZE_SEL=LOW  → Size2
// =============================================================================
#ifndef LCDTAP_LCD_SIZE1_W
#define LCDTAP_LCD_SIZE1_W 240
#endif
#ifndef LCDTAP_LCD_SIZE1_H
#define LCDTAP_LCD_SIZE1_H 320
#endif
#ifndef LCDTAP_LCD_SIZE2_W
#define LCDTAP_LCD_SIZE2_W 320
#endif
#ifndef LCDTAP_LCD_SIZE2_H
#define LCDTAP_LCD_SIZE2_H 240
#endif
