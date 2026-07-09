#pragma once
#include <cstdint>

#include <driver/parlio_rx.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lcdtap/lcdtap.hpp"
#include "lcdtap/m5tab5/spi_deser.hpp"

namespace lcdtap::m5tab5 {

// SPI slave input captured with the PARLIO RX unit:
//   - SCK  -> external clock input (non-free-running)
//   - MOSI -> data lane 0
//   - D/C  -> data lane 1
//   - CS   -> data lanes 2 and 3 (captured in-band, NOT used as the
//             delimiter valid signal: the level delimiter's enable-signal
//             synchronizer runs on the gated SCK domain and drops the
//             first 2 samples of every frame)
// A soft delimiter streams samples continuously; framing (CS gating and
// byte alignment) is done in software by the deserializer in
// spi_deser.hpp. SCK only toggles during transfers, so idle periods
// produce no samples.

struct ParlioSpiSlaveConfig {
  int pinSck;
  int pinMosi;
  int pinDc;
  int pinCs;
  // Self-driven pins used to prime the RX pipeline (both must be
  // electrically unconnected). pinPrime is toggled as a dummy clock;
  // pinPrimeData is driven high and routed to the CS lanes during the
  // injection so the priming samples read as "CS idle".
  int pinPrime;
  int pinPrimeData;
};

struct ParlioSpiSlaveState {
  // Set by caller before parlioSpiSlaveProcess calls:
  lcdtap::LcdTap *inst;
  // Task to notify from the ISR when new data arrives (set by caller):
  TaskHandle_t drainTask;
  // Raw-sample hex dump for bring-up diagnostics (enabled by caller):
  bool rawDumpEnabled;
  volatile uint16_t rawDumpLen;
  uint8_t rawDumpBuf[64];
  // Internal fields:
  ParlioSpiSlaveConfig cfg;
  parlio_rx_unit_handle_t rxUnit;
  parlio_rx_delimiter_handle_t delim;
  uint8_t *dmaBuf;  // driver payload buffer (DMA-capable internal RAM)
  uint32_t dmaBytes;
  uint8_t *rawRing;  // raw sample ring (internal RAM), power-of-two size
  uint32_t rawRingBytes;
  // Free-running indices (wrap via power-of-two masking on access):
  volatile uint32_t rawWriteIdx;
  volatile uint32_t rawReadIdx;
  // Deserializer state (drain-task context):
  SpiDeser deser;
  uint8_t *staging;  // same-D/C data run buffer
  uint32_t stagingBytes;
  uint32_t stagingLen;
  bool active;
  bool started;  // capture transaction has been started at least once
  // Diagnostics:
  volatile uint32_t ringOverflowCount;
  volatile uint32_t isrChunkCount;  // DMA chunks delivered by the ISR
};

// Create and enable the PARLIO RX unit and start an infinite streaming
// receive. rawRing/staging are caller-allocated; rawRingBytes must be a
// power of 2. Only one instance can be active at a time.
esp_err_t parlioSpiSlaveInit(ParlioSpiSlaveState *s,
                             const ParlioSpiSlaveConfig &cfg, uint8_t *rawRing,
                             uint32_t rawRingBytes, uint8_t *staging,
                             uint32_t stagingBytes);

// Stop the receive stream and release the PARLIO unit.
void parlioSpiSlaveDeinit(ParlioSpiSlaveState *s);

// Deserialize pending raw samples and dispatch commands/data to s->inst.
void parlioSpiSlaveProcess(ParlioSpiSlaveState *s);

// Discard all pending captured data (used on RESX assertion).
void parlioSpiSlaveFlush(ParlioSpiSlaveState *s);

// Restart the capture from a clean, byte-aligned state: aborts the
// running transaction, restarts it, and primes the RX pipeline with
// self-generated clock edges so that no real edges are lost to the start
// synchronization. Must only be called while the master is quiet
// (init / capture-stall watchdog).
esp_err_t parlioSpiSlaveRealign(ParlioSpiSlaveState *s);

// Inject a CS-idle barrier into the RUNNING capture stream without
// restarting it, and discard everything ahead of the barrier (data from
// before the reset that is still stuck in the FIFO/DMA pipe). Used on
// RESX assertion; the caller must guarantee the bus is quiet (master in
// reset) and must have drained the ring first.
void parlioSpiSlaveInjectBarrier(ParlioSpiSlaveState *s);

}  // namespace lcdtap::m5tab5
