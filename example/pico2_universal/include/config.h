#pragma once

#include <cstdint>

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
// Dual-chip-select parallel slave pins (parallel_2cs.pio, PIO1 SM0)
//
// For KS0108-style hosts (two controller chips, high-active CS1
// and CS2). One 74HC00 provides the strobe gating; read cycles (busy-flag
// polling) never strobe:
//   CS1  → GPIO 0   (raw, high active; shared with PIN_RST — no reset input
//                    in this mode, the RST GPIO IRQ must be disabled)
//   CS2  → GPIO 1   (raw, high active; shared with PIN_SPI_CS)
//   /EW  → GPIO 2   (= NAND(E, /R/W), /R/W = NAND(R/W, R/W); LOW only while
//                    E is high in a WRITE cycle; shared with PIN_PAR_WR)
//   D[0] → GPIO 3   ... D[7] → GPIO 10  (shared with the parallel bus)
//   D/I  → GPIO 11  (0 = command, 1 = data; shared with PIN_PAR_DC)
//   /RES   not connected
// =============================================================================

// CS1, high active (shared with PIN_RST)
static constexpr uint PIN_PAR2CS_CS1 = 0u;

// CS2, high active (shared with PIN_SPI_CS)
static constexpr uint PIN_PAR2CS_CS2 = 1u;

// /EW write strobe (shared with PIN_PAR_WR; must match PAR2CS_E_PIN defined
// in parallel_2cs.pio)
static constexpr uint PIN_PAR2CS_E = 2u;

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
// DisplayLink USB host pins (PIO USB Full-Speed, D+/D- must be adjacent)
//
// Shared with the parallel bus (D[7] = GPIO10, DC = GPIO11), so DisplayLink
// output is forbidden in PARALLEL bus mode (see outputInterfaceAllowed()).
// pio0 is dedicated to the USB host; SPI/parallel input stays on pio1.
// =============================================================================
static constexpr uint8_t PIN_USB_DP = 10u;
static constexpr uint8_t PIN_USB_DM = 11u;
static constexpr uint8_t USB_PIO_INDEX = 0u;

// =============================================================================
// Boot-time configuration GPIOs (active-low, internal pull-up)
// Default (not connected, pull-up HIGH) = primary mode.
// Driven LOW = alternate mode.
// =============================================================================

// LOW=1280×720@30Hz / HIGH=640×480@60Hz (default)
static constexpr uint PIN_CFG_OUT_RESO_SEL = 20u;

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

// =============================================================================
// Composite output: chroma conversion path
//   0 = RGB332 LUT
//   1 = per-pixel YUV, naive reference (divisions; too slow for real time)
//   2 = per-pixel YUV, optimized Q16 (full RGB565 resolution; the default)
// Mode 2 is validated on hardware for NTSC (both DACs) and PAL R-2R. PAL +
// PWM cannot meet the Core 1 slot deadline with the OSD open, so the driver
// forces that one combination to LUT regardless of this setting (see
// compositeOutInit()). Override via cmake: -DCVBS_CHROMA_MODE=0.
// =============================================================================
#ifndef CVBS_CHROMA_MODE
#define CVBS_CHROMA_MODE 2
#endif
