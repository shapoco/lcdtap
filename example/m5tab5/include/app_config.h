#pragma once

// LcdTap example for M5Stack Tab5 (ESP32-P4)
// Application-wide constants: pin assignment, buffer sizes, task layout.

#include <cstdint>

//=============================================================================
// Pin assignment (M5-Bus / Grove port on the back of the Tab5)
//=============================================================================

// SPI slave input (captured via PARLIO RX)
static constexpr int PIN_SPI_SCK = 17;   // PARLIO external clock input
static constexpr int PIN_SPI_MOSI = 16;  // PARLIO data lane 0
static constexpr int PIN_SPI_DC = 18;    // PARLIO data lane 1
static constexpr int PIN_SPI_CS = 45;    // PARLIO valid signal (active low)

// Reset signal from the master (shared by SPI/I2C modes)
static constexpr int PIN_RESX = 19;

// Self-driven pins for PARLIO capture priming. Only used internally
// through the GPIO matrix; both must be left unconnected.
// PIN_PRIME toggles as a dummy clock at init/realign; PIN_PRIME_DATA is
// held high and fed to the CS lanes during priming so the injected
// samples read as "CS idle".
static constexpr int PIN_PRIME = 52;
static constexpr int PIN_PRIME_DATA = 51;

// I2C slave input (Grove HY2.0-4P port)
static constexpr int PIN_I2C_SDA = 53;
static constexpr int PIN_I2C_SCL = 54;
static constexpr uint8_t I2C_SLAVE_ADDR = 0x3C;

//=============================================================================
// Display output
//=============================================================================

// Number of scanlines rendered and pushed to the panel (via
// gfx->pushImage()) at a time -- just a batching granularity for the
// strip buffer, not tied to any transfer-hardware constraint (the panel
// runs at its native orientation, see main.cpp's setRotation(0), so
// pushImage() takes M5GFX's fast contiguous-memcpy path directly and
// needs no rotation workaround). Tune against the
// `[main] timing us/frame:` log if the strip buffer size vs. pushImage()
// call-count tradeoff turns out to matter.
// 720 x 40 x 2 bytes = 56.25 KB per strip buffer (PSRAM).
static constexpr uint16_t STRIP_LINES = 40;

//=============================================================================
// Input buffers
//=============================================================================

// Raw PARLIO sample ring (2 bits per SCK edge). Power of two.
static constexpr uint32_t SPI_RAW_RING_BYTES = 64u * 1024u;

// PARLIO DMA payload buffer handed to the driver.
static constexpr uint32_t SPI_CHUNK_BYTES = 16u * 1024u;

// Staging buffer for deserialized same-D/C byte runs.
static constexpr uint32_t SPI_STAGING_BYTES = 4096u;

// Dump the first raw sample bytes to serial for bring-up diagnostics
// (bit placement / frame start verification). Re-armed by RESX flush.
static constexpr bool SPI_RAW_DUMP_ENABLE = true;

// Diagnostic I2C bus sniffer: run the PARLIO unit in parallel with the
// I2C slave, sampling SDA on every SCL rising edge, and expose the wire
// bit stream through the raw dump.
static constexpr bool I2C_SNIFF_ENABLE = true;

// I2C word ring ([bit8 = D/C, bits7:0 = byte]). Power of two.
static constexpr uint32_t I2C_RING_BUF_WORDS = 1024u;

//=============================================================================
// Tasks
//=============================================================================

static constexpr int INPUT_TASK_CORE = 0;
static constexpr int DISPLAY_TASK_CORE = 1;
static constexpr uint32_t INPUT_TASK_PRIO = 20;
static constexpr uint32_t DISPLAY_TASK_PRIO = 10;
static constexpr uint32_t INPUT_TASK_STACK = 4096;
static constexpr uint32_t DISPLAY_TASK_STACK = 8192;
