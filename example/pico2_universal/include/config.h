#pragma once

#include <cstdint>

// =============================================================================
// Common pins (all interface modes)
// =============================================================================

// Hardware reset, active low (input, pull-up)
static constexpr uint PIN_RESX = 1u;

// =============================================================================
// 4-Line SPI slave pins (spi_4line_mode0.pio, PIO1 SM0)
//
// Direct connection — no external ICs required.
//   SPI RESX → GPIO 1  (hardware reset, active low, pull-up on Pico 2)
//   SPI SCLK → GPIO 2  (clock, CPOL=0: idle LOW; must match SPI_SCLK_PIN in
//                        spi_4line_mode0.pio)
//   SPI MOSI → GPIO 4  (data MSB first; PIO IN_BASE)
//   SPI DCX  → GPIO 5  (D/C# signal; sampled as IN_BASE+1 in
//   spi_4line_mode0.pio) SPI CS   → GPIO 6  (chip select, active low; must
//   match SPI_CS_PIN in
//                        spi_4line_mode0.pio)
// =============================================================================

// SPI clock (input; must equal SPI_SCLK_PIN defined in spi_4line_mode0.pio = 2)
static constexpr uint PIN_SPI_SCLK = 2u;

// SPI data (input, PIO IN_BASE)
static constexpr uint PIN_SPI_MOSI = 4u;

// D/C# signal (input, PIO IN_BASE+1; 4-line SPI only)
static constexpr uint PIN_SPI_DCX = 5u;

// Chip select, active low (input; must equal SPI_CS_PIN defined in
// spi_4line_mode0.pio = 6)
static constexpr uint PIN_SPI_CS = 6u;

// =============================================================================
// Parallel slave pins (parallel_8bit.pio, PIO1 SM0)
//
// External circuit: 74HC595 (serial-in / parallel-out) + 74HC4040 (÷8) +
// 74AHC1G04 (inverter)
//   SPI SCLK  → 74HC4040 CP, 74HC595 SRCLK
//   SPI MOSI  → 74HC595 SER
//   SPI CS    → 74HC4040 CLR  (active-high; holds Q3=0, BCLK=1 while CS
//               de-asserted)
//   74HC4040 Q3 → 74AHC1G04 → 74HC595 RCLK, GPIO 2  (BCLK = SCLK/8,
//               HIGH when byte complete)
//   74HC595 Q1 → GPIO 4  (D[0], LSB)  ... Q8 → GPIO 11 (D[7], MSB)
//   DCX       → GPIO 3   (D/C# signal, direct from SPI master)
//   RESX      → GPIO 1   (hardware reset, active low)
// =============================================================================

// SCLK/8 byte clock (input)
static constexpr uint PIN_PAR_BCLK = 2u;

// D/C# (input, PIO JMP_PIN)
static constexpr uint PIN_PAR_DCX = 3u;

// D[0..7] (input, PIO IN_BASE; GPIO 4-11)
static constexpr uint PIN_PAR_DATA_BASE = 4u;

// =============================================================================
// I2C slave pins (I2C0, software slave mode)
//
// Note: GPIO 8-9 overlap with PAR_DATA[4:5] in Parallel mode.
//       Only one interface is physically wired at a time.
//   I2C SDA → GPIO 8
//   I2C SCL → GPIO 9
// =============================================================================

static constexpr uint PIN_I2C_SDA = 8u;
static constexpr uint PIN_I2C_SCL = 9u;
static constexpr uint I2C_SLAVE_ADDR = 0x3Cu;

// =============================================================================
// Boot-time configuration GPIOs (read once at startup, pull-down)
// =============================================================================

// LOW=640x480@60Hz / HIGH=1280x720@30Hz(reduced)
static constexpr uint PIN_CFG_DVI_RES = 21u;

// =============================================================================
// Key inputs (active-low, internal pull-up)
// =============================================================================
static constexpr uint PIN_KEY_UP = 20u;
static constexpr uint PIN_KEY_DOWN = 22u;
static constexpr uint PIN_KEY_LEFT = 26u;
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
// I2C ring buffer
// Filled by IRQ handler; drained by main loop.
// Same word format: bit[8]=DCX, bits[7:0]=data byte.
// =============================================================================
static constexpr uint32_t I2C_RING_BUF_WORDS = 256u;  // 1 KB

// =============================================================================
// DVI scanline buffers (RGB565, fed to PicoDVI q_colour_valid)
// Must be <= q_colour_free queue depth (8, set in dvi_init).
// DVI_MAX_W is the maximum effective width (h_active_pixels/2) across all
// supported DVI timings. Used to statically size the scanline buffers.
// =============================================================================
static constexpr int N_SCANLINE_BUFS = 4;
static constexpr uint32_t DVI_MAX_W = 640u;  // 1280x720: 1280/2 = 640

// =============================================================================
// LED blink interval (DVI output frames)
// =============================================================================
static constexpr uint32_t LED_TOGGLE_FRAMES = 30u;

// =============================================================================
// Memory pool for the LcdTap framebuffer (sole user of the pool).
// Scanline buffers are now statically allocated (see DVI_MAX_W above).
// Sized to fit the largest practical framebuffer: 320x480 RGB565 = 307 200 B.
// Requests that exceed the pool return nullptr; updateConfig() keeps the
// existing framebuffer in that case.
// =============================================================================
static constexpr size_t MEM_POOL_SIZE = 310u * 1024u;

// =============================================================================
// Framebuffer size defaults (overridable via cmake -DLCDTAP_LCD_SIZE_W=...
// etc.) Adjustable at runtime via the OSD menu.
// =============================================================================
#ifndef LCDTAP_LCD_SIZE_W
#define LCDTAP_LCD_SIZE_W 240
#endif
#ifndef LCDTAP_LCD_SIZE_H
#define LCDTAP_LCD_SIZE_H 320
#endif
