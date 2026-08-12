#pragma once

#include <cstdint>

// =============================================================================
// Input pins — identical to example/pico2_universal so the same wiring works
// on both boards. On the Pico 2 W GPIO 23/24/25/29 belong to the CYW43; none
// of the input pins touch them, and the LED is the CYW43's WL_GPIO0.
// =============================================================================

// Hardware reset, active low (input, pull-up)
static constexpr uint PIN_RST = 0u;

// =============================================================================
// 4-Line SPI slave pins (spi_4line_mode0.pio, PIO1 SM0)
//
//   RST  → GPIO 0  (hardware reset, active low)
//   CS   → GPIO 1  (chip select, active low; must match SPI_CS_PIN)
//   SCLK → GPIO 2  (clock, CPOL=0; must match SPI_SCLK_PIN)
//   MOSI → GPIO 3  (data MSB first; PIO IN_BASE)
//   DC   → GPIO 4  (D/C# signal; IN_BASE+1)
// =============================================================================
static constexpr uint PIN_SPI_CS = 1u;
static constexpr uint PIN_SPI_SCLK = 2u;
static constexpr uint PIN_SPI_MOSI = 3u;
static constexpr uint PIN_SPI_DC = 4u;

// =============================================================================
// Parallel slave pins (parallel_8bit.pio, PIO1 SM0)
//
//   RST  → GPIO 0   CS  → GPIO 1   WR# → GPIO 2
//   D[0] → GPIO 3 ... D[7] → GPIO 10   DC → GPIO 11
// =============================================================================
static constexpr uint PIN_PAR_CS = 1u;
static constexpr uint PIN_PAR_WR = 2u;
static constexpr uint PIN_PAR_DATA_BASE = 3u;
static constexpr uint PIN_PAR_DC = 11u;

// =============================================================================
// Dual-chip-select parallel slave pins (parallel_2cs.pio, PIO1 SM0)
//
// For KS0108 / SG12864-style hosts (two controller chips, high-active CS1
// and CS2). One 74HC00 provides the strobe gating; read cycles never strobe:
//   CS1  → GPIO 0   (raw, high active; shared with PIN_RST — no reset input
//                    in this mode, the RST GPIO IRQ must be disabled)
//   CS2  → GPIO 1   (raw, high active; shared with PIN_SPI_CS)
//   /EW  → GPIO 2   (= NAND(E, /R/W), /R/W = NAND(R/W, R/W); LOW only while
//                    E is high in a WRITE cycle; shared with PIN_PAR_WR)
//   D[0] → GPIO 3 ... D[7] → GPIO 10   D/I → GPIO 11
//   /RES   not connected
// =============================================================================
static constexpr uint PIN_PAR2CS_CS1 = 0u;
static constexpr uint PIN_PAR2CS_CS2 = 1u;
static constexpr uint PIN_PAR2CS_E = 2u;

// =============================================================================
// I2C slave pins (I2C0)
// =============================================================================
static constexpr uint PIN_I2C_SDA = 8u;
static constexpr uint PIN_I2C_SCL = 9u;
static constexpr uint I2C_SLAVE_ADDR = 0x3Cu;

// =============================================================================
// PIO / DMA resource assignment
// pio1 is used for SPI/Parallel slave interfaces; the CYW43 driver claims a
// free SM on another PIO dynamically.
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
// =============================================================================
static constexpr uint32_t I2C_RING_BUF_WORDS = 256u;  // 1 KB

// =============================================================================
// Memory pool for the LcdTap framebuffer (sole user of the pool).
// Sized to fit the largest practical framebuffer: 320x480 RGB565 = 307 200 B.
// =============================================================================
static constexpr size_t MEM_POOL_SIZE = 310u * 1024u;

// =============================================================================
// Framebuffer size defaults (overridable via cmake -DLCDTAP_LCD_SIZE_W= etc.)
// =============================================================================
#ifndef LCDTAP_LCD_SIZE_W
#define LCDTAP_LCD_SIZE_W 240
#endif
#ifndef LCDTAP_LCD_SIZE_H
#define LCDTAP_LCD_SIZE_H 320
#endif

// =============================================================================
// Flash layout: each config blob occupies one sector, addressed from the end
// of flash. Sector 1 is the LcdTap device config, sector 2 the network
// config, so they can be written independently.
// =============================================================================
static constexpr uint32_t DEVICE_CONFIG_SECTORS_FROM_END = 1u;
static constexpr uint32_t NET_CONFIG_SECTORS_FROM_END = 2u;

// =============================================================================
// HTTP server
// =============================================================================
static constexpr uint16_t HTTP_PORT = 80u;
