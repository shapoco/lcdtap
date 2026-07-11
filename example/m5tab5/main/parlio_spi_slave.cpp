// SPI slave input driver for ESP32-P4 using the PARLIO RX unit.
//
// Capture scheme (see also spi_deser.hpp and parlio_spi_slave.hpp):
//   - SCK drives the PARLIO RX unit as a gated external clock; every
//     rising edge samples 4 lanes (MOSI, D/C, CS, CS-duplicate).
//   - A soft delimiter streams samples continuously into DMA. The level
//     delimiter is deliberately NOT used: its enable-signal synchronizer
//     is clocked by the gated SCK, so the first 2 samples after each CS
//     assertion are lost (observed as a 2-bit shift of every byte).
//     Capturing CS in-band and framing in software avoids the enable
//     path entirely, like the pico2 PIO capture.
//   - Zero-copy: with indirect_mount=0 the ESP-IDF driver mounts dmaBuf
//     directly to the DMA descriptors, performs NO memcpy, and
//     esp_cache_msync()s each finished descriptor before invoking
//     on_partial_receive (verified in esp-idf v5.5 parlio_rx.c,
//     parlio_rx_default_desc_done_callback). The ISR only queues the
//     chunk's (offset, length); the drain task decodes in arrival order.
//
// Why chunk references instead of a linear ring (lesson from a failed
// revision): the soft-delimiter EOF closes each DMA descriptor early, so
// consecutive chunks do NOT sit at consecutive buffer offsets — they sit
// at their descriptors' addresses (the driver carves the buffer into
// nodes of up to ~4 KB), leaving holes where a descriptor was closed
// before it was full. Reading the buffer linearly interleaves stale hole
// bytes into the stream. edata->data/recv_bytes are the ground truth.
//
// Also note (same source dive): indirect_mount=1 makes the driver memcpy
// each chunk into the user payload in the ISR, but its destination
// offset is reset by the per-EOF callback, so the payload buffer is NOT
// a contiguous ring either — reading it positionally is unsound. Copying
// out of edata->data in the ISR (the previous working design) was the
// only sound use of it, and that costs a 31 MB/s memcpy at full rate.
//
// Overrun semantics: the DMA descriptor ring wraps regardless of the
// consumer, so if the drain task falls behind by about a buffer's worth
// of samples the referenced data is overwritten in place. Each chunk
// carries a cumulative byte position; the drain drops chunks whose data
// may have been lapped (counted in ringOverflowCount) and the
// deserializer realigns on the next CS idle samples.
//
// Known trade-off: without the CS-deassert EOF flush, the tail of a
// burst becomes visible only when the DMA node / soft-delimiter frame it
// belongs to completes (bounded by SOFT_EOF_BYTES). Continuous masters
// are unaffected.

#include "lcdtap/m5tab5/parlio_spi_slave.hpp"
#include "lcdtap/m5tab5/ram_pool.hpp"

#include <cstring>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <soc/parlio_periph.h>

namespace lcdtap::m5tab5 {

namespace {
constexpr const char *TAG = "parlio_spi_slave";
}

static constexpr uint32_t EXT_CLK_FREQ_HZ = 62'500'000;  // max supported SCK

// Soft-delimiter frame length in raw bytes (2 samples/byte). Bounds the
// EOF interrupt rate at full speed and the worst-case tail latency.
// 4032 matches the driver's 64-byte-aligned max DMA node size, so most
// frames close their descriptor exactly full: fewer split chunks/holes
// in dmaBuf (better lap tolerance) and half the interrupt rate compared
// to 2048. Correctness does not depend on this matching — chunks are
// consumed by explicit (offset, length) references.
static constexpr uint32_t SOFT_EOF_BYTES = 4032;

// A queued chunk is considered lapped (overwritten by the wrapping DMA)
// once this many bytes were captured after it. The DMA descriptor ring
// covers dmaBufBytes minus the per-cycle EOF holes; keep a conservative
// margin below that.
static constexpr uint32_t STALE_MARGIN_BYTES = 16u * 1024u;

//=============================================================================
// ISR callback
//=============================================================================

static bool IRAM_ATTR onPartialReceive(parlio_rx_unit_handle_t,
                                       const parlio_rx_event_data_t *edata,
                                       void *userData) {
  ParlioSpiSlaveState *s = static_cast<ParlioSpiSlaveState *>(userData);
  s->isrChunkCount = s->isrChunkCount + 1;
  const uint8_t *chunk = static_cast<const uint8_t *>(edata->data);
  uint32_t len = (uint32_t)edata->recv_bytes;
  if (chunk >= s->dmaBuf && chunk + len <= s->dmaBuf + s->dmaBufBytes) {
    uint32_t cum = s->isrCumBytes + len;
    s->isrCumBytes = cum;
    uint32_t wr = s->chunkWr;
    if (wr - s->chunkRd < SPI_CHUNK_QUEUE_LEN) {
      SpiChunkRef &ref = s->chunkQueue[wr & (SPI_CHUNK_QUEUE_LEN - 1u)];
      ref.offset = (uint32_t)(chunk - s->dmaBuf);
      ref.len = len;
      ref.cumEnd = cum;
      s->chunkWr = wr + 1;
    } else {
      // Queue full = consumer more than a whole buffer behind; the data
      // of the oldest references is overwritten anyway. Drop this chunk
      // (never touch chunkRd from the ISR: single-consumer invariant).
      s->ringOverflowCount = s->ringOverflowCount + 1;
    }
  } else {
    // Not expected: the driver reported a pointer outside dmaBuf.
    s->isrDesyncCount = s->isrDesyncCount + 1;
  }
  BaseType_t woken = pdFALSE;
  if (s->drainTask) vTaskNotifyGiveFromISR(s->drainTask, &woken);
  return woken != pdFALSE;
}

//=============================================================================
// Init / deinit
//=============================================================================

esp_err_t parlioSpiSlaveInit(ParlioSpiSlaveState *s,
                             const ParlioSpiSlaveConfig &cfg,
                             uint32_t dmaBufBytes, uint8_t *staging,
                             uint32_t stagingBytes) {
  if (dmaBufBytes < 2 * SOFT_EOF_BYTES || dmaBufBytes % CACHE_ALIGN != 0) {
    ESP_LOGE(TAG, "dmaBufBytes=%u invalid", (unsigned)dmaBufBytes);
    return ESP_ERR_INVALID_ARG;
  }

  s->cfg = cfg;
  s->rxUnit = nullptr;
  s->delim = nullptr;
  s->dmaBufBytes = dmaBufBytes;
  s->chunkWr = 0;
  s->chunkRd = 0;
  s->isrCumBytes = 0;
  spiDeserInit(&s->deser);
  s->sink.dataBuf = staging;
  s->sink.dataCap = stagingBytes;
  s->sink.dataLen = 0;
  s->sink.onDataFull = [](void *user) {
    ParlioSpiSlaveState *st = static_cast<ParlioSpiSlaveState *>(user);
    st->inst->inputData(st->sink.dataBuf, st->sink.dataLen, 1);
  };
  s->sink.onCommand = [](void *user, uint8_t byte) {
    // The deserializer has already flushed the pending data run.
    ParlioSpiSlaveState *st = static_cast<ParlioSpiSlaveState *>(user);
    st->inst->inputCommand(byte);
  };
  s->sink.user = s;
  s->ringOverflowCount = 0;
  s->isrChunkCount = 0;
  s->isrDesyncCount = 0;
  s->rawDumpLen = 0;
  s->started = false;

  // Pool-owned buffer (see ram_pool.hpp/.cpp), not heap-allocated here.
  s->dmaBuf = (uint8_t *)spiDmaMemPool;
  if (!s->dmaBuf) {
    ESP_LOGE(TAG, "dmaBuf alloc failed");
    return ESP_ERR_NO_MEM;
  }

  parlio_rx_unit_config_t unitCfg = {};
  unitCfg.trans_queue_depth = 4;
  unitCfg.max_recv_size = dmaBufBytes;
  unitCfg.data_width = 4;
  unitCfg.clk_src = PARLIO_CLK_SRC_EXTERNAL;
  unitCfg.ext_clk_freq_hz = EXT_CLK_FREQ_HZ;
  unitCfg.exp_clk_freq_hz = EXT_CLK_FREQ_HZ;
  unitCfg.clk_in_gpio_num = (gpio_num_t)cfg.pinSck;
  unitCfg.clk_out_gpio_num = GPIO_NUM_NC;
  unitCfg.valid_gpio_num = GPIO_NUM_NC;  // CS is captured in-band instead
  for (size_t i = 0; i < PARLIO_RX_UNIT_MAX_DATA_WIDTH; ++i) {
    unitCfg.data_gpio_nums[i] = GPIO_NUM_NC;
  }
  unitCfg.data_gpio_nums[0] = (gpio_num_t)cfg.pinMosi;
  unitCfg.data_gpio_nums[1] = (gpio_num_t)cfg.pinDc;
  unitCfg.data_gpio_nums[2] = (gpio_num_t)cfg.pinCs;
  unitCfg.data_gpio_nums[3] = (gpio_num_t)cfg.pinCs;  // lane 3 unused
  unitCfg.flags.free_clk = 0;  // SCK toggles only while transferring

  esp_err_t err = parlio_new_rx_unit(&unitCfg, &s->rxUnit);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "parlio_new_rx_unit failed: %d", (int)err);
    goto fail;
  }

  // CS idles high (active-low). Without this, a realign that happens to
  // run while the host hasn't connected yet (e.g. the capture watchdog's
  // periodic re-realign firing before the master ever started) can prime
  // against a floating CS line; if it settles low, the deserializer's
  // discardUntilIdle barrier -- which only clears on a genuine CS-idle
  // sample -- never clears, and the interface is stuck decoding nothing
  // for the rest of the session even after the master starts for real.
  // Matches the existing pico2 SPI slave, which pulls up its CS pin for
  // the same reason (example/pico2_common/src/spi_slave.cpp).
  gpio_set_pull_mode((gpio_num_t)cfg.pinCs, GPIO_PULLUP_ONLY);

  {
    parlio_rx_soft_delimiter_config_t delimCfg = {};
    delimCfg.sample_edge = PARLIO_SAMPLE_EDGE_POS;  // SPI mode 0
    delimCfg.bit_pack_order = PARLIO_BIT_PACK_ORDER_MSB;
    delimCfg.eof_data_len = SOFT_EOF_BYTES;
    delimCfg.timeout_ticks = 0;  // no hardware timeout
    err = parlio_new_rx_soft_delimiter(&delimCfg, &s->delim);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "parlio_new_rx_soft_delimiter failed: %d", (int)err);
      goto fail;
    }
  }

  {
    parlio_rx_event_callbacks_t cbs = {};
    cbs.on_partial_receive = onPartialReceive;
    cbs.on_receive_done = nullptr;  // fires per soft EOF; data comes in chunks
    cbs.on_timeout = nullptr;
    err = parlio_rx_unit_register_event_callbacks(s->rxUnit, &cbs, s);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "parlio_rx_unit_register_event_callbacks failed: %d",
               (int)err);
      goto fail;
    }
  }

  // Priming pin: self-driven dummy clock, output with input path enabled
  // so the GPIO matrix can feed it back to the PARLIO inputs.
  // pinPrime = dummy clock (idles low); pinPrimeData = constant high fed
  // to the CS lanes during priming.
  {
    gpio_config_t primeCfg = {};
    primeCfg.pin_bit_mask = (1ull << cfg.pinPrime) | (1ull << cfg.pinPrimeData);
    primeCfg.mode = GPIO_MODE_INPUT_OUTPUT;
    err = gpio_config(&primeCfg);
    if (err != ESP_OK) goto fail;
    gpio_set_level((gpio_num_t)cfg.pinPrime, 0);
    gpio_set_level((gpio_num_t)cfg.pinPrimeData, 1);
  }

  err = parlioSpiSlaveRealign(s);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "parlioSpiSlaveRealign failed: %d", (int)err);
    goto fail;
  }

  s->active = true;
  return ESP_OK;

fail:
  parlioSpiSlaveDeinit(s);
  return err;
}

// Reroute clk_in_sig and the CS data lanes to the self-driven priming
// pins, disconnecting them from the real master pins. Split out from
// primePipeline() so parlioSpiSlaveRealign() can call this BEFORE
// starting the receive: clk_in_sig stays wired to the real pinSck from
// parlio_new_rx_unit() onward, so if the reroute happened only after
// starting the receive (as an earlier revision did), there was a window
// where the RX unit's start-of-stream synchronizer could still see live
// edges from a master that never idles its SCK (e.g. CoreS3, PicoSystem)
// -- a race whose outcome depended on boot-time timing, observed on
// hardware as a probabilistic "screen sometimes never comes up" failure
// (the synchronizer locks onto a real edge instead of a dummy one, the
// deserializer's discardUntilIdle then never clears because such
// masters hold CS asserted continuously, and everything is silently
// discarded via the slow per-sample path for the rest of the session).
// Calling this first makes the real pins physically unreachable from
// clk_in_sig for the whole priming sequence, turning the race into a
// deterministic guarantee.
static void primeRerouteIn(ParlioSpiSlaveState *s) {
  const auto &sigs = parlio_periph_signals.groups[0].rx_units[0];
  const gpio_num_t primePin = (gpio_num_t)s->cfg.pinPrime;
  const gpio_num_t primeDataPin = (gpio_num_t)s->cfg.pinPrimeData;

  esp_rom_gpio_connect_in_signal(primeDataPin, sigs.data_sigs[2], false);
  esp_rom_gpio_connect_in_signal(primeDataPin, sigs.data_sigs[3], false);
  esp_rom_gpio_connect_in_signal(primePin, sigs.clk_in_sig, false);
}

// Inject dummy clock edges from the priming pin so that the RX pipeline's
// start synchronization consumes them instead of the master's first real
// edges (clk_in_sig/CS lanes must already be rerouted to the priming
// pins via primeRerouteIn() before calling this). Every priming sample
// reads as "CS idle" (pinPrimeData is driven high), so the deserializer
// drops them naturally (no counting needed) and they double as the
// in-stream realign barrier that ends the discardUntilIdle window.
// Reroutes clk_in_sig/CS lanes back to the real pins afterward.
// Note: GPIO_MATRIX_CONST_ONE_INPUT has no effect on the PARLIO RX data
// inputs on the ESP32-P4 (verified on hardware), hence the real pin.
static void primeInjectAndRerouteBack(ParlioSpiSlaveState *s) {
  const auto &sigs = parlio_periph_signals.groups[0].rx_units[0];
  const gpio_num_t primePin = (gpio_num_t)s->cfg.pinPrime;

  // Any number of edges works (self-discarding); a few extra beyond the
  // pipeline's synchronization depth.
  constexpr uint32_t PRIME_EDGES = 32;

  for (uint32_t i = 0; i < PRIME_EDGES; ++i) {
    gpio_set_level(primePin, 1);
    esp_rom_delay_us(1);
    gpio_set_level(primePin, 0);
    esp_rom_delay_us(1);
  }
  esp_rom_gpio_connect_in_signal((gpio_num_t)s->cfg.pinSck, sigs.clk_in_sig,
                                 false);
  esp_rom_gpio_connect_in_signal((gpio_num_t)s->cfg.pinCs, sigs.data_sigs[2],
                                 false);
  esp_rom_gpio_connect_in_signal((gpio_num_t)s->cfg.pinCs, sigs.data_sigs[3],
                                 false);
}

// Reroute + inject + reroute back, for use on an already-running capture
// (parlioSpiSlaveInjectBarrier()) where there is no start-of-stream
// synchronizer event to race against.
static void primePipeline(ParlioSpiSlaveState *s) {
  primeRerouteIn(s);
  primeInjectAndRerouteBack(s);
}

esp_err_t parlioSpiSlaveRealign(ParlioSpiSlaveState *s) {
  if (!s->rxUnit || !s->delim) return ESP_ERR_INVALID_STATE;

  // Stop the running transaction. Note that samples already inside the
  // FIFO/DMA pipe are NOT dropped — they are delivered later, in order,
  // ahead of new samples. The deserializer therefore discards everything
  // up to the priming barrier (discardUntilIdle) instead of relying on
  // positional flushing.
  if (s->started) {
    parlio_rx_soft_delimiter_start_stop(s->rxUnit, s->delim, false);
    parlio_rx_unit_disable(s->rxUnit);
  }

  // The next receive remounts the descriptor ring from scratch; clear
  // the chunk queue. The ISR is quiescent here (unit disabled or not yet
  // started).
  s->chunkWr = 0;
  s->chunkRd = 0;
  s->isrCumBytes = 0;
  spiDeserReset(&s->deser);
  s->deser.discardUntilIdle = true;
  s->sink.dataLen = 0;
  s->rawDumpLen = 0;  // re-arm the bring-up dump

  esp_err_t err = parlio_rx_unit_enable(s->rxUnit, true);
  if (err != ESP_OK) return err;

  // Reroute clk_in_sig/CS lanes to the priming pins BEFORE starting the
  // receive below, so the RX unit's start-of-stream synchronizer can
  // only ever see dummy edges -- see primeRerouteIn()'s comment for why
  // the previous ordering (reroute after starting the receive) raced
  // against continuously-clocking masters.
  primeRerouteIn(s);

  parlio_receive_config_t recvCfg = {};
  recvCfg.delimiter = s->delim;
  recvCfg.flags.partial_rx_en = 1;
  // Direct mount: dmaBuf is attached to the DMA descriptors as-is and
  // the driver ISR does not copy anything (see file header).
  recvCfg.flags.indirect_mount = 0;
  err = parlio_rx_unit_receive(s->rxUnit, s->dmaBuf, s->dmaBufBytes, &recvCfg);
  if (err == ESP_OK) {
    err = parlio_rx_soft_delimiter_start_stop(s->rxUnit, s->delim, true);
  }
  if (err != ESP_OK) {
    parlio_rx_unit_disable(s->rxUnit);
    return err;
  }
  s->started = true;

  primeInjectAndRerouteBack(s);
  return ESP_OK;
}

void parlioSpiSlaveInjectBarrier(ParlioSpiSlaveState *s) {
  if (!s->active) return;
  // Everything currently in flight (FIFO/DMA pipe) belongs to the time
  // before the barrier and is dropped; the injected CS-idle samples end
  // the discard window and realign the bit accumulator.
  s->deser.discardUntilIdle = true;
  s->sink.dataLen = 0;
  s->rawDumpLen = 0;  // re-arm the bring-up dump
  primePipeline(s);
}

void parlioSpiSlaveDeinit(ParlioSpiSlaveState *s) {
  if (s->rxUnit) {
    if (s->delim) {
      parlio_rx_soft_delimiter_start_stop(s->rxUnit, s->delim, false);
    }
    parlio_rx_unit_disable(s->rxUnit);
  }
  if (s->delim) {
    parlio_del_rx_delimiter(s->delim);
    s->delim = nullptr;
  }
  if (s->rxUnit) {
    parlio_del_rx_unit(s->rxUnit);
    s->rxUnit = nullptr;
  }
  if (s->dmaBuf) {
    // Pool-owned (see ram_pool.hpp/.cpp); not freed here.
    s->dmaBuf = nullptr;
  }
  s->active = false;
}

//=============================================================================
// Drain (input-task context)
//=============================================================================

void parlioSpiSlaveProcess(ParlioSpiSlaveState *s) {
  uint32_t wr = s->chunkWr;  // snapshot of volatile
  uint32_t rd = s->chunkRd;
  if (rd == wr) return;

  const uint32_t staleAge = s->dmaBufBytes - STALE_MARGIN_BYTES;
  while (rd != wr) {
    // Copy the reference; the slot is not reused until chunkRd advances.
    SpiChunkRef ref = s->chunkQueue[rd & (SPI_CHUNK_QUEUE_LEN - 1u)];
    ++rd;
    if (s->isrCumBytes - ref.cumEnd > staleAge) {
      // The wrapping DMA has likely overwritten this chunk already.
      s->ringOverflowCount = s->ringOverflowCount + 1;
      s->chunkRd = rd;
      continue;
    }
    const uint8_t *raw = s->dmaBuf + ref.offset;

    if (s->rawDumpEnabled && s->rawDumpLen < sizeof(s->rawDumpBuf)) {
      uint32_t n = sizeof(s->rawDumpBuf) - s->rawDumpLen;
      if (n > ref.len) n = ref.len;
      memcpy(s->rawDumpBuf + s->rawDumpLen, raw, n);
      s->rawDumpLen = s->rawDumpLen + n;
    }

    // Without an instance (e.g. bus-sniffer mode) only the dump is kept.
    if (s->inst) spiDeserProcess(&s->deser, raw, ref.len, &s->sink);
    s->chunkRd = rd;
  }
  if (!s->inst) return;

  spiDeserFlushData(&s->sink);
}

void parlioSpiSlaveFlush(ParlioSpiSlaveState *s) {
  s->chunkRd = s->chunkWr;
  spiDeserReset(&s->deser);
  s->sink.dataLen = 0;
  s->rawDumpLen = 0;  // re-arm the bring-up dump
}

}  // namespace lcdtap::m5tab5
