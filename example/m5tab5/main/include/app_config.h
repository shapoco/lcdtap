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

// Number of scanlines rendered and DMA-transferred (async_blit.cpp, via
// esp_async_memcpy) to the panel at a time. The panel runs at its native
// orientation (see main.cpp's setRotation(0)), so the DMA engine does a
// straight (unrotated) copy per strip -- unlike an earlier rotated
// design, the output block here is just `numLines` rows starting at row
// `logicalY`, so there's no fixed "always full panel height" row-crossing
// cost to amortize by going bigger. What this value does control is
// pipeline granularity: small enough that the CPU (fill+strip, a few
// hundred us/strip) reliably finishes well before the DMA transfer
// (~1.5ms/strip at this size) so fill work stays fully hidden behind the
// transfer, per the ping-pong double buffering in display_out.cpp. Tune
// against the `[main] timing us/frame:` log (wait/dmaBusy in particular)
// if revisiting.
// 720 x 40 x 2 bytes = 56.25 KB per strip buffer (internal SRAM
// preferred, PSRAM fallback; aligned to CONFIG_CACHE_L2_CACHE_LINE_SIZE;
// two buffers are allocated for ping-pong CPU-fill/DMA-transfer overlap,
// see display_out.hpp).
static constexpr uint16_t STRIP_LINES = 40;

//=============================================================================
// Input buffers
//=============================================================================

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
// Diagnostics
//=============================================================================

// One-shot micro-benchmark run at the very start of setup() (before
// M5.begin(), before any ISR/task exists) to measure whether the
// ESP32-P4 HP core executes a plain (potentially misaligned) 32-bit
// pointer-cast load at native speed or via a slow/trapping emulation
// path. Result (2026-07-11, M5Tab5 hardware): off=0 6.00 cy/word,
// off=1..3 7-8 cy/word (1.17-1.33x) -- native HW support confirmed, not
// a trap. spi_deser.hpp's bulk decoder was simplified to a single
// unconditional path on the strength of this result (see
// example/m5tab5/tmp.improve-performance.md). Leave false; only enable
// for a dedicated bring-up flash if this ever needs re-verifying on
// different hardware.
static constexpr bool BENCH_UNALIGNED_LOAD = false;

//=============================================================================
// Tasks
//=============================================================================

static constexpr int INPUT_TASK_CORE = 0;
static constexpr int DISPLAY_TASK_CORE = 1;
static constexpr uint32_t INPUT_TASK_PRIO = 20;
static constexpr uint32_t DISPLAY_TASK_PRIO = 10;
static constexpr uint32_t INPUT_TASK_STACK = 4096;
static constexpr uint32_t DISPLAY_TASK_STACK = 8192;
