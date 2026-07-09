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
// This header is free of MCU dependencies so the logic can be verified
// with a host-compiled unit test. If the empirical bit placement differs,
// only the masks/shifts here need adjustment.

namespace lcdtap::m5tab5 {

static constexpr uint8_t SPI_DESER_MOSI_MASK = 0x11;
static constexpr uint8_t SPI_DESER_DC_MASK = 0x22;
static constexpr uint8_t SPI_DESER_CS_MASK = 0x44;

using SpiDeserEmitFn = void (*)(void *user, uint8_t byte, bool dc);

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
  uint32_t emitCount;            // decoded bytes handed to the emit callback
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

inline void spiDeserEmit(SpiDeser *d, uint8_t byte, bool dc,
                         SpiDeserEmitFn emit, void *user) {
  if (d->discardUntilIdle) return;
  ++d->emitCount;
  emit(user, byte, dc);
}

// Process one 4-bit sample (bit0 = MOSI, bit1 = D/C, bit2 = CS wire level).
inline void spiDeserSample(SpiDeser *d, uint8_t nibble, SpiDeserEmitFn emit,
                           void *user) {
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
    spiDeserEmit(d, d->curByte, (nibble >> 1) & 1u, emit, user);
    d->curByte = 0;
    d->bitCount = 0;
  }
}

// Process a linear run of raw sample bytes (2 samples per byte).
// The fast path emits one payload byte per 4 raw bytes while CS stays
// active and the accumulator is byte-aligned.
inline void spiDeserProcess(SpiDeser *d, const uint8_t *raw, uint32_t len,
                            SpiDeserEmitFn emit, void *user) {
  uint32_t i = 0;
  while (i < len) {
    if (d->bitCount == 0 && !d->discardUntilIdle && len - i >= 4) {
      uint8_t r0 = raw[i], r1 = raw[i + 1], r2 = raw[i + 2], r3 = raw[i + 3];
      if (((r0 | r1 | r2 | r3) & SPI_DESER_CS_MASK) == 0) {
        // 8 consecutive CS-active samples: one whole payload byte.
        if (!d->inFrame) {
          d->inFrame = true;
          ++d->frameStartCount;
        }
        uint8_t byte = (uint8_t)((((r0 >> 4) & 1u) << 7) | ((r0 & 1u) << 6) |
                                 (((r1 >> 4) & 1u) << 5) | ((r1 & 1u) << 4) |
                                 (((r2 >> 4) & 1u) << 3) | ((r2 & 1u) << 2) |
                                 (((r3 >> 4) & 1u) << 1) | (r3 & 1u));
        spiDeserEmit(d, byte, (r3 >> 1) & 1u, emit, user);
        i += 4;
        continue;
      }
    }
    uint8_t b = raw[i++];
    spiDeserSample(d, (b >> 4) & 0xFu, emit, user);
    spiDeserSample(d, b & 0xFu, emit, user);
  }
}

}  // namespace lcdtap::m5tab5
