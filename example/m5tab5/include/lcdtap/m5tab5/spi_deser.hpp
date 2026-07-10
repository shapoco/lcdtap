#pragma once
#include <cstdint>

// SPI sample-stream deserializer for the PARLIO 4-lane capture.
//
// The PARLIO RX unit samples 4 lanes on every SCK rising edge:
//   lane0 = MOSI, lane1 = D/C, lane2 = CS, lane3 = CS (duplicate, ignored)
// With PARLIO_BIT_PACK_ORDER_MSB one raw byte holds 2 samples packed from
// the MSB side:
//
//   raw bit:    7    6    5    4    3    2    1    0
//   content:  CS'0  CS0  DC0 MOSI0 CS'1 CS1  DC1 MOSI1   (0 = earlier)
//
// CS is captured in-band instead of gating the sampling (the PARLIO level
// delimiter loses the first 2 samples of every frame to its enable-signal
// synchronizer), so framing is done here in software: a CS=1 sample resets
// the bit accumulator, CS=0 samples shift MOSI bits in MSB-first, and a
// payload byte is emitted every 8 bits with the D/C value of its last bit.
//
// Throughput: at the maximum 62.5 MHz SCK the raw stream is 31.25 MB/s
// (7.8 M payload bytes/s), so the decoder must spend only a handful of
// cycles per payload byte. The bulk path therefore reads 4 raw bytes
// (= 8 samples = 1 payload byte) as one little-endian 32-bit word and
// extracts all 8 MOSI bits with a branch-free shift tree, and decoded
// bytes are appended directly to a caller-owned run buffer (SpiDeserSink)
// instead of going through a per-byte callback.
//
// This header is free of MCU dependencies so the logic can be verified
// with a host-compiled unit test. If the empirical bit placement differs,
// only the masks/shifts here need adjustment.

namespace lcdtap::m5tab5 {

static constexpr uint8_t SPI_DESER_MOSI_MASK = 0x11;
static constexpr uint8_t SPI_DESER_DC_MASK = 0x22;
static constexpr uint8_t SPI_DESER_CS_MASK = 0x44;

// Decoded-byte sink. Data bytes (D/C=1) are appended to dataBuf by the
// deserializer itself; the callbacks fire only on the rare boundaries:
//   - onDataFull: dataBuf reached dataCap; consume dataBuf[0..dataLen).
//     The deserializer resets dataLen to 0 after the call returns.
//   - onCommand: a D/C=0 byte arrived. Any pending data run is flushed
//     via onDataFull first, so byte ordering is preserved.
// The caller flushes the final partial run after spiDeserProcess returns.
struct SpiDeserSink {
  uint8_t *dataBuf;  // same-D/C data run buffer (caller-owned)
  uint32_t dataCap;  // capacity of dataBuf in bytes (>= 1)
  uint32_t dataLen;  // current fill level (always < dataCap on entry)
  void (*onDataFull)(void *user);
  void (*onCommand)(void *user, uint8_t byte);
  void *user;
};

struct SpiDeser {
  uint8_t curByte;   // MOSI bits accumulated so far (MSB first)
  uint8_t bitCount;  // number of bits in curByte (0..7)
  bool inFrame;      // last seen CS sample was active (wire low = 0)
  // Suppress emission until the next CS idle sample. Set after a capture
  // realign: the priming sequence injects CS=1 samples into the stream,
  // which act as an in-stream barrier — everything before the barrier is
  // stale data left in the DMA pipe from before the realign.
  bool discardUntilIdle;
  // Diagnostics:
  uint32_t frameStartCount;      // CS idle -> active transitions
  uint32_t partialBitDropCount;  // partial bytes discarded at CS deassert
  uint32_t emitCount;            // decoded bytes handed to the sink
};

inline void spiDeserReset(SpiDeser *d) {
  d->curByte = 0;
  d->bitCount = 0;
  d->inFrame = false;
}

inline void spiDeserInit(SpiDeser *d) {
  spiDeserReset(d);
  d->discardUntilIdle = false;
  d->frameStartCount = 0;
  d->partialBitDropCount = 0;
  d->emitCount = 0;
}

inline void spiDeserFlushData(SpiDeserSink *sink) {
  if (sink->dataLen) {
    sink->onDataFull(sink->user);
    sink->dataLen = 0;
  }
}

inline void spiDeserEmit(SpiDeser *d, SpiDeserSink *sink, uint8_t byte,
                         bool dc) {
  ++d->emitCount;
  if (dc) {
    sink->dataBuf[sink->dataLen++] = byte;
    if (sink->dataLen >= sink->dataCap) spiDeserFlushData(sink);
  } else {
    spiDeserFlushData(sink);  // preserve ordering against the data run
    sink->onCommand(sink->user, byte);
  }
}

// Process one 4-bit sample (bit0 = MOSI, bit1 = D/C, bit2 = CS wire level).
// Slow path: only used around CS transitions, partial bytes and buffer
// tails; the steady state is handled by the bulk path in spiDeserProcess.
inline void spiDeserSample(SpiDeser *d, uint8_t nibble, SpiDeserSink *sink) {
  if (nibble & 0x4u) {
    // CS deasserted (wire high): discard a partial byte and realign.
    if (d->inFrame) {
      if (d->bitCount) ++d->partialBitDropCount;
      d->curByte = 0;
      d->bitCount = 0;
      d->inFrame = false;
    }
    d->discardUntilIdle = false;  // realign barrier reached
    return;
  }
  if (!d->inFrame) {
    d->inFrame = true;
    ++d->frameStartCount;
  }
  d->curByte = (uint8_t)((d->curByte << 1) | (nibble & 1u));
  if (++d->bitCount == 8) {
    if (!d->discardUntilIdle) {
      spiDeserEmit(d, sink, d->curByte, (nibble >> 1) & 1u);
    }
    d->curByte = 0;
    d->bitCount = 0;
  }
}

// Raw-word access types for the bulk path. may_alias permits reading the
// uint8_t sample stream as 32-bit words; the aligned(1) variant tells the
// compiler the address may be unaligned (frame starts land on arbitrary
// raw-byte offsets, and the resulting phase persists for the whole
// burst). GCC lowers the unaligned variant to byte loads on strict-
// alignment targets, which is still far cheaper than the per-sample path.
typedef uint32_t __attribute__((may_alias)) SpiDeserWord;
typedef uint32_t __attribute__((may_alias, aligned(1))) SpiDeserWordU;

// Bulk decode loop: one payload byte per 32-bit word (= 8 samples) while
// CS stays active. Returns the new raw index; stops at a word containing
// a CS idle sample or at the last whole word. WORD selects the aligned
// (single word load) or unaligned flavor.
template <typename WORD>
inline uint32_t spiDeserBulk(SpiDeser *d, const uint8_t *raw, uint32_t i,
                             uint32_t len, SpiDeserSink *sink) {
  // Keep the per-byte state in locals so the hot loop stays free of
  // memory round-trips; sync back on the rare callback boundaries and on
  // exit. The callbacks never touch dataBuf/dataCap, only consume runs.
  uint8_t *buf = sink->dataBuf;
  const uint32_t bufCap = sink->dataCap;
  uint32_t bufLen = sink->dataLen;
  uint32_t emitted = 0;
  while (len - i >= 4) {
    uint32_t w = *reinterpret_cast<const WORD *>(raw + i);
    if (w & 0x44444444u) break;  // a CS idle sample: slow path
    // 8 consecutive CS-active samples = one whole payload byte.
    // MOSI bits sit at word bits {4,0,12,8,20,16,28,24} for samples
    // s0..s7 (earlier sample in the high nibble of each raw byte).
    // Gather them MSB-first (s0 -> bit7) with a shift tree; all
    // intermediate positions are alias-free under the masks.
    uint32_t m = w & 0x11111111u;
    uint32_t p = (m | (m >> 3)) & 0x03030303u;
    uint8_t byte = (uint8_t)((p << 6) | (p >> 4) | (p >> 14) | (p >> 24));
    i += 4;
    ++emitted;
    if (w & (1u << 25)) {  // D/C of the last sample (s7)
      buf[bufLen++] = byte;
      if (bufLen >= bufCap) {
        sink->dataLen = bufLen;
        spiDeserFlushData(sink);
        bufLen = 0;
      }
    } else {
      if (bufLen) {
        sink->dataLen = bufLen;
        spiDeserFlushData(sink);
        bufLen = 0;
      }
      sink->onCommand(sink->user, byte);
    }
  }
  sink->dataLen = bufLen;
  if (emitted) {
    d->emitCount += emitted;
    // CS stays active for the whole loop, so at most one frame starts.
    if (!d->inFrame) {
      d->inFrame = true;
      ++d->frameStartCount;
    }
  }
  return i;
}

// Process a linear run of raw sample bytes (2 samples per byte).
// The bulk path decodes one payload byte per 32-bit word while CS stays
// active and the accumulator is byte-aligned.
inline void spiDeserProcess(SpiDeser *d, const uint8_t *raw, uint32_t len,
                            SpiDeserSink *sink) {
  uint32_t i = 0;
  while (i < len) {
    if (d->bitCount == 0 && !d->discardUntilIdle) {
      if ((((uintptr_t)raw + i) & 3u) == 0) {
        i = spiDeserBulk<SpiDeserWord>(d, raw, i, len, sink);
      } else {
        i = spiDeserBulk<SpiDeserWordU>(d, raw, i, len, sink);
      }
      if (i >= len) break;
    }
    // Slow path: one raw byte (2 samples, earlier one in the high nibble).
    uint8_t b = raw[i++];
    spiDeserSample(d, (uint8_t)(b >> 4), sink);
    spiDeserSample(d, b & 0xFu, sink);
  }
}

}  // namespace lcdtap::m5tab5
