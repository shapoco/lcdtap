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
//
// Zero-copy capture (verified against the ESP-IDF v5.5 parlio_rx.c
// sources): the receive buffer is mounted directly to the DMA
// descriptors (indirect_mount=0), so the driver performs NO memcpy at
// all, and it esp_cache_msync()s every finished descriptor before the
// on_partial_receive callback, so the chunk data is CPU-coherent.
// IMPORTANT: the soft-delimiter EOF (every SOFT_EOF_BYTES) closes DMA
// descriptors EARLY, so the sample stream is NOT laid out contiguously
// in the buffer — each chunk lands at its descriptor's buffer address,
// potentially leaving unused holes behind. (An earlier revision assumed
// a contiguous ring and read interleaved garbage.) The ISR therefore
// queues each chunk's exact (offset, length) as reported by the driver,
// and the drain task decodes the chunks in arrival order.

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

// One finished DMA chunk inside dmaBuf, as reported by the driver ISR.
struct SpiChunkRef {
  uint32_t offset;  // chunk start inside dmaBuf
  uint32_t len;     // chunk length in bytes
  uint32_t cumEnd;  // isrCumBytes right after this chunk (staleness check)
};

// Chunk-reference queue length (power of two). Sized so that the queue
// can hold more chunks than the DMA buffer can hold data; a full queue
// therefore implies the referenced data has been lapped already.
static constexpr uint32_t SPI_CHUNK_QUEUE_LEN = 64;

// Soft-delimiter frame length in raw bytes (2 samples/byte), mirrored
// from parlio_spi_slave.cpp so callers can approximate the SCK rate from
// isrChunkCount (chunks are usually exactly this size for a continuous
// master).
static constexpr uint32_t SPI_SOFT_EOF_BYTES = 4032;

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
  // Receive buffer mounted directly to the RX DMA descriptors (internal
  // DMA-capable RAM, cache-line aligned).
  uint8_t *dmaBuf;
  uint32_t dmaBufBytes;
  // Chunk-reference queue: single producer (PARLIO ISR), single consumer
  // (drain task), both pinned to the same core. Free-running indices.
  SpiChunkRef chunkQueue[SPI_CHUNK_QUEUE_LEN];
  volatile uint32_t chunkWr;
  volatile uint32_t chunkRd;
  volatile uint32_t isrCumBytes;  // total bytes reported by the ISR
  // Deserializer state and decoded-byte sink (drain-task context). The
  // sink's dataBuf is the caller-provided staging buffer.
  SpiDeser deser;
  SpiDeserSink sink;
  bool active;
  bool started;  // capture transaction has been started at least once
  // Diagnostics:
  // Chunks dropped as stale before spiDeserProcess() runs on them (the
  // DMA writer has likely lapped the chunk already; see
  // parlioSpiSlaveProcess()'s staleness check).
  volatile uint32_t ringOverflowCount;
  volatile uint32_t isrChunkCount;   // DMA chunks reported by the ISR
  volatile uint32_t isrDesyncCount;  // chunk pointer outside dmaBuf
};

// Create and enable the PARLIO RX unit and start an infinite streaming
// receive into an internally allocated DMA buffer of dmaBufBytes.
// staging is the caller-allocated data run buffer. Only one instance can
// be active at a time.
esp_err_t parlioSpiSlaveInit(ParlioSpiSlaveState *s,
                             const ParlioSpiSlaveConfig &cfg,
                             uint32_t dmaBufBytes, uint8_t *staging,
                             uint32_t stagingBytes);

// Stop the receive stream and release the PARLIO unit.
void parlioSpiSlaveDeinit(ParlioSpiSlaveState *s);

// Deserialize pending raw samples and dispatch commands/data to s->inst.
void parlioSpiSlaveProcess(ParlioSpiSlaveState *s);

// Discard all pending captured data (used on RST assertion).
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
// RST assertion; the caller must guarantee the bus is quiet (master in
// reset) and must have drained the ring first.
void parlioSpiSlaveInjectBarrier(ParlioSpiSlaveState *s);

}  // namespace lcdtap::m5tab5
