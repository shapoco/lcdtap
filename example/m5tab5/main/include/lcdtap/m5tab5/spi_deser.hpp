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
  // Upper bound (in samples) on how long discardUntilIdle waits for a
  // genuine CS-idle sample before the caller forces it clear anyway (0 =
  // unbounded, wait for CS idle only). Some masters assert CS once at
  // boot and never release it for the whole session (a single-drop SPI
  // bus has no reason to), in which case the intended CS-idle barrier
  // can never arrive on its own; without a bound, one realign/barrier
  // that happens to land before such a master starts wedges decoding
  // for the rest of the boot even once the master is actively sending.
  // A real CS-idle sample still takes priority and clears the barrier
  // immediately whenever one does show up.
  uint32_t discardBudget;
  // Diagnostics:
  uint32_t frameStartCount;      // CS idle -> active transitions
  uint32_t partialBitDropCount;  // partial bytes discarded at CS deassert
  uint32_t emitCount;            // decoded bytes handed to the sink
  uint32_t forcedRecoveryCount;  // discardBudget reached 0 before CS idle
};

inline void spiDeserReset(SpiDeser *d) {
  d->curByte = 0;
  d->bitCount = 0;
  d->inFrame = false;
}

inline void spiDeserInit(SpiDeser *d) {
  spiDeserReset(d);
  d->discardUntilIdle = false;
  d->discardBudget = 0;
  d->frameStartCount = 0;
  d->partialBitDropCount = 0;
  d->emitCount = 0;
  d->forcedRecoveryCount = 0;
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
  if (d->discardUntilIdle && d->discardBudget != 0 && --d->discardBudget == 0) {
    d->discardUntilIdle = false;
    ++d->forcedRecoveryCount;
  }
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

// Raw-word access type for the bulk path. may_alias permits reading the
// uint8_t sample stream as 32-bit words without violating strict
// aliasing. Deliberately NOT packed/aligned(1): a naturally-aligned type
// makes GCC emit a single unconditional `lw`, even though the actual
// pointer may be misaligned at runtime (UB per the standard). This is
// safe here because the ESP32-P4 HP core executes misaligned 32-bit
// loads in hardware at near-native speed (micro-benchmarked on real
// M5Tab5 hardware, 2026-07-11: 6.00 cy/word aligned vs 7-8 cy/word
// misaligned, i.e. 1.17-1.33x, not a trap -- see
// example/m5tab5/tmp.improve-performance.md and
// example/m5tab5/include/app_config.h's BENCH_UNALIGNED_LOAD). An
// earlier revision split this into an aligned/unaligned dual path
// (SpiDeserWord/SpiDeserWordU selected by a runtime address check) to
// avoid the UB; that turned out to be actively harmful, not just
// unnecessary: because a whole CS-held frame's word-decode alignment is
// fixed at frame start (an accident of DMA/EOF chunking timing) and
// never changes for the rest of the frame, an unlucky session could get
// permanently locked onto the (then much slower, byte-load) unaligned
// path for its entire duration, overrunning the CPU budget badly enough
// to starve the FreeRTOS idle task into a watchdog reset on real
// hardware (M5Stack CoreS3 as master). Do NOT reintroduce
// packed/aligned(1) here, and do not enable -fsanitize=alignment on
// this header, without re-running the micro-benchmark on whatever
// target you're adding.
struct SpiDeserWord {
  uint32_t v;
} __attribute__((may_alias));

// Decode one 32-bit raw word (= 8 samples with CS active) into its
// payload byte. MOSI bits sit at word bits {4,0,12,8,20,16,28,24} for
// samples s0..s7 (earlier sample in the high nibble of each raw byte).
// Gather them MSB-first (s0 -> bit7) with a shift tree; all intermediate
// positions are alias-free under the masks.
[[gnu::always_inline]] inline uint8_t spiDeserDecodeWord(uint32_t w) {
  uint32_t m = w & 0x11111111u;
  uint32_t p = (m | (m >> 3)) & 0x03030303u;
  return (uint8_t)((p << 6) | (p >> 4) | (p >> 14) | (p >> 24));
}

// Bulk decode loop: one payload byte per 32-bit word (= 8 samples) while
// CS stays active. The steady state (a data burst with D/C=1) is handled
// 4 words at a time so the CS/DC tests and the loop overhead are shared
// across 4 payload bytes. Returns the new raw index; stops at a word
// containing a CS idle sample or at the last whole word. `raw+i` may be
// misaligned at runtime; see the SpiDeserWord comment above for why that
// is fine on this target.
inline uint32_t spiDeserBulk(SpiDeser *d, const uint8_t *raw, uint32_t i,
                             uint32_t len, SpiDeserSink *sink) {
  // Keep the per-byte state in locals so the hot loop stays free of
  // memory round-trips; sync back on the rare callback boundaries and on
  // exit. The callbacks never touch dataBuf/dataCap, only consume runs.
  uint8_t *buf = sink->dataBuf;
  const uint32_t bufCap = sink->dataCap;
  uint32_t bufLen = sink->dataLen;
  uint32_t emitted = 0;
  const bool quadOk = bufCap >= 4;  // quad path stores 4 bytes at once
  while (len - i >= 4) {
    uint32_t w0 = reinterpret_cast<const SpiDeserWord *>(raw + i)->v;
    if (quadOk && len - i >= 16) {
      uint32_t w1 = reinterpret_cast<const SpiDeserWord *>(raw + i + 4)->v;
      uint32_t w2 = reinterpret_cast<const SpiDeserWord *>(raw + i + 8)->v;
      uint32_t w3 = reinterpret_cast<const SpiDeserWord *>(raw + i + 12)->v;
      // Quad fast path: 32 samples with CS active and D/C=1 throughout
      // (bit25 = D/C of each word's last sample; D/C is byte-uniform on
      // the wire during data runs, so testing the last sample per word
      // matches the per-word emit semantics).
      if (((w0 | w1 | w2 | w3) & 0x44444444u) == 0 &&
          (((w0 & w1 & w2 & w3) >> 25) & 1u) != 0) {
        if (bufLen + 4 > bufCap) {  // flush early; run splitting is harmless
          sink->dataLen = bufLen;
          spiDeserFlushData(sink);
          bufLen = 0;
        }
        buf[bufLen + 0] = spiDeserDecodeWord(w0);
        buf[bufLen + 1] = spiDeserDecodeWord(w1);
        buf[bufLen + 2] = spiDeserDecodeWord(w2);
        buf[bufLen + 3] = spiDeserDecodeWord(w3);
        bufLen += 4;
        if (bufLen >= bufCap) {
          sink->dataLen = bufLen;
          spiDeserFlushData(sink);
          bufLen = 0;
        }
        i += 16;
        emitted += 4;
        continue;
      }
      // CS idle or a D/C=0 byte somewhere in the quad: fall through and
      // handle w0 alone, then retry the quad from the next word.
    }
    if (w0 & 0x44444444u) break;  // a CS idle sample: slow path
    uint8_t byte = spiDeserDecodeWord(w0);
    i += 4;
    ++emitted;
    if (w0 & (1u << 25)) {  // D/C of the last sample (s7)
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
      i = spiDeserBulk(d, raw, i, len, sink);
      if (i >= len) break;
    }
    // Slow path: one raw byte (2 samples, earlier one in the high nibble).
    uint8_t b = raw[i++];
    spiDeserSample(d, (uint8_t)(b >> 4), sink);
    spiDeserSample(d, b & 0xFu, sink);
  }
}

}  // namespace lcdtap::m5tab5
