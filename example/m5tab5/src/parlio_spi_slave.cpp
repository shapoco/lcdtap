// SPI slave input driver for ESP32-P4 using the PARLIO RX unit.
//
// Capture scheme (see also spi_deser.hpp):
//   - SCK drives the PARLIO RX unit as a gated external clock; every
//     rising edge samples 4 lanes (MOSI, D/C, CS, CS-duplicate).
//   - A soft delimiter streams samples continuously into DMA. The level
//     delimiter is deliberately NOT used: its enable-signal synchronizer
//     is clocked by the gated SCK, so the first 2 samples after each CS
//     assertion are lost (observed as a 2-bit shift of every byte).
//     Capturing CS in-band and framing in software avoids the enable
//     path entirely, like the pico2 PIO capture.
//   - The driver's on_partial_receive callback copies each finished DMA
//     node into a raw ring buffer. on_receive_done is NOT used for data:
//     in partial mode its event data points at the whole user buffer
//     with a cumulative byte count, so pushing it would duplicate data.
//
// Known trade-off: without the CS-deassert EOF flush, the tail of a
// burst becomes visible only when the DMA node / soft-delimiter frame it
// belongs to completes (bounded by SPI_SOFT_EOF_BYTES). Continuous
// masters are unaffected.

#include "lcdtap/m5tab5/parlio_spi_slave.hpp"

#include <cstring>

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <soc/parlio_periph.h>

namespace lcdtap::m5tab5 {

static constexpr uint32_t EXT_CLK_FREQ_HZ = 62'500'000;  // max supported SCK

// Soft-delimiter frame length in raw bytes (2 samples/byte). Bounds both
// the EOF interrupt rate at full speed and the worst-case tail latency.
static constexpr uint32_t SOFT_EOF_BYTES = 2048;

//=============================================================================
// ISR callback
//=============================================================================

static void IRAM_ATTR pushRaw(ParlioSpiSlaveState *s, const uint8_t *data,
                              uint32_t len) {
  if (!len) return;
  uint32_t mask = s->rawRingBytes - 1u;
  uint32_t wr = s->rawWriteIdx;
  uint32_t used = wr - s->rawReadIdx;
  if (used + len > s->rawRingBytes) {
    // Consumer too slow; drop the chunk. The deserializer realigns on the
    // next CS idle samples.
    ++s->ringOverflowCount;
    return;
  }
  uint32_t first = s->rawRingBytes - (wr & mask);
  if (first > len) first = len;
  memcpy(s->rawRing + (wr & mask), data, first);
  if (len > first) memcpy(s->rawRing, data + first, len - first);
  s->rawWriteIdx = wr + len;
}

static bool IRAM_ATTR onPartialReceive(parlio_rx_unit_handle_t,
                                       const parlio_rx_event_data_t *edata,
                                       void *userData) {
  ParlioSpiSlaveState *s = static_cast<ParlioSpiSlaveState *>(userData);
  ++s->isrChunkCount;
  pushRaw(s, static_cast<const uint8_t *>(edata->data),
          (uint32_t)edata->recv_bytes);
  BaseType_t woken = pdFALSE;
  if (s->drainTask) vTaskNotifyGiveFromISR(s->drainTask, &woken);
  return woken != pdFALSE;
}

//=============================================================================
// Init / deinit
//=============================================================================

esp_err_t parlioSpiSlaveInit(ParlioSpiSlaveState *s,
                             const ParlioSpiSlaveConfig &cfg, uint8_t *rawRing,
                             uint32_t rawRingBytes, uint8_t *staging,
                             uint32_t stagingBytes) {
  s->cfg = cfg;
  s->rxUnit = nullptr;
  s->delim = nullptr;
  s->rawRing = rawRing;
  s->rawRingBytes = rawRingBytes;
  s->rawWriteIdx = 0;
  s->rawReadIdx = 0;
  spiDeserInit(&s->deser);
  s->staging = staging;
  s->stagingBytes = stagingBytes;
  s->stagingLen = 0;
  s->ringOverflowCount = 0;
  s->isrChunkCount = 0;
  s->rawDumpLen = 0;
  s->started = false;

  s->dmaBytes = 16u * 1024u;
  s->dmaBuf = (uint8_t *)heap_caps_aligned_calloc(
      64, 1, s->dmaBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!s->dmaBuf) return ESP_ERR_NO_MEM;

  parlio_rx_unit_config_t unitCfg = {};
  unitCfg.trans_queue_depth = 4;
  unitCfg.max_recv_size = s->dmaBytes;
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
  if (err != ESP_OK) goto fail;

  {
    parlio_rx_soft_delimiter_config_t delimCfg = {};
    delimCfg.sample_edge = PARLIO_SAMPLE_EDGE_POS;  // SPI mode 0
    delimCfg.bit_pack_order = PARLIO_BIT_PACK_ORDER_MSB;
    delimCfg.eof_data_len = SOFT_EOF_BYTES;
    delimCfg.timeout_ticks = 0;  // no hardware timeout
    err = parlio_new_rx_soft_delimiter(&delimCfg, &s->delim);
    if (err != ESP_OK) goto fail;
  }

  {
    parlio_rx_event_callbacks_t cbs = {};
    cbs.on_partial_receive = onPartialReceive;
    cbs.on_receive_done = nullptr;  // would duplicate data in partial mode
    cbs.on_timeout = nullptr;
    err = parlio_rx_unit_register_event_callbacks(s->rxUnit, &cbs, s);
    if (err != ESP_OK) goto fail;
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
  if (err != ESP_OK) goto fail;

  s->active = true;
  return ESP_OK;

fail:
  parlioSpiSlaveDeinit(s);
  return err;
}

// Inject dummy clock edges from the priming pin so that the RX pipeline's
// start synchronization consumes them instead of the master's first real
// edges. The CS data lanes are rerouted to pinPrimeData (driven high)
// during the injection, so every priming sample reads as "CS idle": the
// deserializer drops them naturally (no counting needed) and they double
// as the in-stream realign barrier that ends the discardUntilIdle window.
// Note: GPIO_MATRIX_CONST_ONE_INPUT has no effect on the PARLIO RX data
// inputs on the ESP32-P4 (verified on hardware), hence the real pin.
static void primePipeline(ParlioSpiSlaveState *s) {
  const auto &sigs = parlio_periph_signals.groups[0].rx_units[0];
  const gpio_num_t primePin = (gpio_num_t)s->cfg.pinPrime;
  const gpio_num_t primeDataPin = (gpio_num_t)s->cfg.pinPrimeData;

  // Any number of edges works (self-discarding); a few extra beyond the
  // pipeline's synchronization depth.
  constexpr uint32_t PRIME_EDGES = 32;

  esp_rom_gpio_connect_in_signal(primeDataPin, sigs.data_sigs[2], false);
  esp_rom_gpio_connect_in_signal(primeDataPin, sigs.data_sigs[3], false);
  esp_rom_gpio_connect_in_signal(primePin, sigs.clk_in_sig, false);
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

  s->rawReadIdx = s->rawWriteIdx;
  spiDeserReset(&s->deser);
  s->deser.discardUntilIdle = true;
  s->stagingLen = 0;
  s->rawDumpLen = 0;  // re-arm the bring-up dump

  esp_err_t err = parlio_rx_unit_enable(s->rxUnit, true);
  if (err != ESP_OK) return err;

  parlio_receive_config_t recvCfg = {};
  recvCfg.delimiter = s->delim;
  recvCfg.flags.partial_rx_en = 1;
  // Let the driver mount its own internal DMA buffer and copy chunks out
  // in the ISR; this is the documented configuration for infinite
  // transactions.
  recvCfg.flags.indirect_mount = 1;
  err = parlio_rx_unit_receive(s->rxUnit, s->dmaBuf, s->dmaBytes, &recvCfg);
  if (err == ESP_OK) {
    err = parlio_rx_soft_delimiter_start_stop(s->rxUnit, s->delim, true);
  }
  if (err != ESP_OK) {
    parlio_rx_unit_disable(s->rxUnit);
    return err;
  }
  s->started = true;

  primePipeline(s);
  return ESP_OK;
}

void parlioSpiSlaveInjectBarrier(ParlioSpiSlaveState *s) {
  if (!s->active) return;
  // Everything currently in flight (FIFO/DMA pipe) belongs to the time
  // before the barrier and is dropped; the injected CS-idle samples end
  // the discard window and realign the bit accumulator.
  s->deser.discardUntilIdle = true;
  s->stagingLen = 0;
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
    heap_caps_free(s->dmaBuf);
    s->dmaBuf = nullptr;
  }
  s->active = false;
}

//=============================================================================
// Drain (input-task context)
//=============================================================================

static void emitByte(void *user, uint8_t byte, bool dc) {
  ParlioSpiSlaveState *s = static_cast<ParlioSpiSlaveState *>(user);
  if (dc) {
    s->staging[s->stagingLen++] = byte;
    if (s->stagingLen >= s->stagingBytes) {
      s->inst->inputData(s->staging, s->stagingLen, 1);
      s->stagingLen = 0;
    }
  } else {
    if (s->stagingLen) {
      s->inst->inputData(s->staging, s->stagingLen, 1);
      s->stagingLen = 0;
    }
    s->inst->inputCommand(byte);
  }
}

void parlioSpiSlaveProcess(ParlioSpiSlaveState *s) {
  uint32_t wr = s->rawWriteIdx;  // snapshot of volatile
  uint32_t rd = s->rawReadIdx;
  if (rd == wr) return;
  if (!s->inst) {
    s->rawReadIdx = wr;
    return;
  }

  uint32_t mask = s->rawRingBytes - 1u;
  while (rd != wr) {
    // Process the largest linear (non-wrapping) run.
    uint32_t len = wr - rd;
    uint32_t linear = s->rawRingBytes - (rd & mask);
    if (len > linear) len = linear;
    const uint8_t *raw = s->rawRing + (rd & mask);

    if (s->rawDumpEnabled && s->rawDumpLen < sizeof(s->rawDumpBuf)) {
      uint32_t n = sizeof(s->rawDumpBuf) - s->rawDumpLen;
      if (n > len) n = len;
      memcpy(s->rawDumpBuf + s->rawDumpLen, raw, n);
      s->rawDumpLen = s->rawDumpLen + n;
    }

    spiDeserProcess(&s->deser, raw, len, emitByte, s);
    rd += len;
  }
  s->rawReadIdx = rd;

  if (s->stagingLen) {
    s->inst->inputData(s->staging, s->stagingLen, 1);
    s->stagingLen = 0;
  }
}

void parlioSpiSlaveFlush(ParlioSpiSlaveState *s) {
  s->rawReadIdx = s->rawWriteIdx;
  spiDeserReset(&s->deser);
  s->stagingLen = 0;
  s->rawDumpLen = 0;  // re-arm the bring-up dump
}

}  // namespace lcdtap::m5tab5
