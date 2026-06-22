#pragma once

#include <cstdint>

// =============================================================================
// SPI slave pins (SPI mode, PIO1 SM0)
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
// I2C slave pins (I2C mode, I2C0)
//
// SSD1306 I2C address: 0x3C (SA0 tied low)
// Connect SSD1306 SDA/SCL directly to Pico 2; add 4.7kΩ pull-ups to 3.3V.
// =============================================================================
static constexpr uint PIN_I2C_SDA = 8u;
static constexpr uint PIN_I2C_SCL = 9u;
static constexpr uint I2C_SLAVE_ADDR = 0x3Cu;

// =============================================================================
// Boot-time configuration GPIOs (active-low, internal pull-up)
// Default (not connected, pull-up HIGH) = primary mode.
// Driven LOW = alternate mode.
// =============================================================================

// LOW=SPI mode / HIGH=I2C mode (default)
static constexpr uint PIN_CFG_IFACE_SEL = 22u;

// LOW=1280×720@30Hz / HIGH=640×480@60Hz (default)
static constexpr uint PIN_CFG_OUT_720P = 20u;

// LOW=Size2 / HIGH=Size1 (default)
static constexpr uint PIN_CFG_LCD_SIZE_SEL = 21u;

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
// PIO / DMA resource assignment (SPI mode)
// pio1 is used for the SPI slave.
// =============================================================================
#define SPI_PIO pio1
static constexpr uint SPI_SM = 0u;

// =============================================================================
// SPI ring buffer (SPI mode)
// Must be a power-of-two number of bytes and aligned to its own size.
// Each element is one uint32_t word: bit[8]=DC, bits[7:0]=data byte.
// =============================================================================
static constexpr uint32_t SPI_RING_BUF_LOG2 = 12u;  // 4KB (SSD1306 is small)
static constexpr uint32_t SPI_RING_BUF_BYTES = 1u << SPI_RING_BUF_LOG2;
static constexpr uint32_t SPI_RING_BUF_WORDS =
    SPI_RING_BUF_BYTES / sizeof(uint32_t);

// =============================================================================
// I2C ring buffer (I2C mode)
// Filled by IRQ handler; drained by main loop.
// Same word format as SPI ring buffer: bit[8]=DC, bits[7:0]=data byte.
// =============================================================================
static constexpr uint32_t I2C_RING_BUF_WORDS = 256u;  // 1KB

// =============================================================================
// LED blink interval (DVI output frames)
// =============================================================================
static constexpr uint32_t LED_TOGGLE_FRAMES = 30u;

// =============================================================================
// Memory pool for the LcdTap framebuffer (sole user of the pool).
// Scanline buffers are now statically allocated (see DVI_MAX_W above).
// SSD1306 framebuffer is 128x64 RGB565 = 16 384 bytes; 32 KB is sufficient.
// =============================================================================
static constexpr size_t MEM_POOL_SIZE = 32u * 1024u;

// =============================================================================
// Framebuffer size defaults (overridable via cmake -DLCDTAP_LCD_SIZE1_W= etc.)
// PIN_CFG_LCD_SIZE_SEL=HIGH → Size1 (default)
// PIN_CFG_LCD_SIZE_SEL=LOW  → Size2
// =============================================================================
#ifndef LCDTAP_LCD_SIZE1_W
#define LCDTAP_LCD_SIZE1_W 128
#endif
#ifndef LCDTAP_LCD_SIZE1_H
#define LCDTAP_LCD_SIZE1_H 64
#endif
#ifndef LCDTAP_LCD_SIZE2_W
#define LCDTAP_LCD_SIZE2_W 128
#endif
#ifndef LCDTAP_LCD_SIZE2_H
#define LCDTAP_LCD_SIZE2_H 32
#endif
