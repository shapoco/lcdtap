#pragma once

// Composite video sample encoding. This layer has no MCU dependencies so that
// a whole field can be generated and verified on a host machine before any of
// it is run against an oscilloscope.
//
// Sample packing
// --------------
// Two DAC sinks with different transfer formats share this encoder:
//
//   R-2R : the PIO shifts right out of the OSR with an autopull threshold of
//          flushBits, so several samples are bit-packed into one 32-bit DMA
//          transfer, sample 0 in the least significant bits:
//            word = s0 | s1 << bits | s2 << (2*bits) | ...
//          Only floor(32 / bits) samples fit; the remaining high bits are
//          discarded by the autopull and never shifted out.
//   PWM  : one 16-bit counter-compare value per DMA transfer, no packing.
//
// The difference is confined to a policy chosen at init; the line structure
// below is written once. compositeEmitLine() dispatches through a function
// pointer in CompositeEncoder, so callers see no difference.
//
// Line grouping
// -------------
// samplesPerLine is not generally a multiple of samplesPerTransfer (NTSC 910,
// PAL 1135), so for the R-2R sink a single line does not occupy a whole
// number of transfers. The DMA unit is therefore a *group* of lines:
//
//   alignLines   = samplesPerTransfer / gcd(samplesPerTransfer, samplesPerLine)
//   linesPerSlot = lcm(alignLines, COMPOSITE_MIN_LINES_PER_SLOT)
//
// The PWM sink has no alignment constraint (alignLines == 1), but a group of
// one line would leave Core 1 only NUM_SLOTS-1 lines of fill budget, so the
// minimum keeps both sinks at the same safety margin.
//
// The writer runs continuously from the start of a group to its end, so no
// code below ever has to reason about alignment.

#include <cstdint>

#include "lcdtap/pico2/composite_timing.hpp"

namespace lcdtap::pico2 {

// Upper bound on linesPerSlot across all supported timing/DAC combinations
// (worst case is PAL with the R-2R DAC: gcd(4, 1135) = 1).
static constexpr uint32_t COMPOSITE_MAX_LINES_PER_SLOT = 4u;

// Floor on linesPerSlot, so that Core 1 always has at least
// (NUM_SLOTS-1) * this many lines of fill budget regardless of sink.
static constexpr uint32_t COMPOSITE_MIN_LINES_PER_SLOT = 2u;

// Colours are quantized to RGB332 before chroma encoding.
static constexpr uint32_t COMPOSITE_LUT_COLORS = 256u;

enum class CompositeLineType : uint8_t {
  EQ,             // equalizing pulses (two narrow sync pulses)
  SERR,           // vertical sync (two broad pulses)
  BLANK_NOBURST,  // blanked line, burst suppressed
  BLANK_BURST,    // blanked line with colour burst
  ACTIVE,         // burst plus active video
};

// Everything the line generator needs, derived once from a timing table and a
// DAC profile. Levels held here are *physical* DAC codes, so the inner loops
// never touch the normalized level space.
struct CompositeSampleWriter;
struct CompositeEncoder;

using CompositeEmitLineFn = void (*)(CompositeSampleWriter *w,
                                     const CompositeEncoder *e,
                                     CompositeLineType type,
                                     const uint16_t *px);

struct CompositeEncoder {
  const CompositeTiming *timing;
  const CompositeDacProfile *dac;

  // Transfer geometry. A "transfer" is one DMA beat: a packed word for the
  // R-2R sink, a single counter-compare value for PWM.
  uint32_t samplesPerTransfer;  // 4 (R-2R 7-bit) / 1 (PWM)
  uint32_t bytesPerTransfer;    // 4 / 2
  uint32_t linesPerSlot;
  uint32_t transfersPerSlot;
  uint32_t bytesPerSlot;

  // Bit-packing parameters; used by the R-2R policy only.
  uint32_t bitsPerSample;
  uint32_t flushBits;

  // Emits one line in this sink's transfer format.
  CompositeEmitLineFn emitLine;

  // Physical DAC codes for the constant levels
  uint32_t codeSyncTip;
  uint32_t codeBlank;
  uint32_t codeWhite;
  uint32_t codeMax;

  // Burst level per subcarrier phase index (absolute sample index & 3)
  uint32_t burstCode[COMPOSITE_NUM_PHASES];

  // lut[set][color]: two packed 7-bit-space samples per half.
  //   bits [13:0]  = the pixel's two samples when it starts at phase `set`
  //   bits [29:16] = the same pixel when it starts at phase `set + 2`
  // Sets are indexed by subcarrier phase; PAL colour additionally uses the
  // upper sets for the inverted-V lines.
  uint32_t *lut;
  uint32_t lutSets;
};

// Sequential sample writer. `sampleIndex` counts from the start of the group
// and drives subcarrier phase, so nothing needs to be transfer-aligned.
// acc/nbits are used by the bit-packing policy only.
struct CompositeSampleWriter {
  union {
    uint32_t *w;  // R-2R: packed words
    uint16_t *h;  // PWM: one counter-compare value per sample
  } dst;
  uint32_t acc;
  uint32_t nbits;
  uint32_t sampleIndex;
  uint32_t bitsPerSample;
  uint32_t samplesPerTransfer;
  uint32_t flushBits;
};

// Compute the derived geometry, the physical level codes and the chroma LUT.
// `lutStorage` must hold at least compositeLutWords(timing) uint32_t.
// Returns false if the combination is unsupported.
bool compositeEncoderInit(CompositeEncoder *e, const CompositeTiming *timing,
                          const CompositeDacProfile *dac, uint32_t *lutStorage);

// Number of uint32_t the caller must provide for the LUT.
uint32_t compositeLutWords(const CompositeTiming *timing);

// Geometry of one line group, for the given timing and DAC profile.
uint32_t compositeLinesPerSlot(const CompositeTiming *timing,
                               const CompositeDacProfile *dac);
uint32_t compositeTransfersPerSlot(const CompositeTiming *timing,
                                   const CompositeDacProfile *dac);
uint32_t compositeBytesPerSlot(const CompositeTiming *timing,
                               const CompositeDacProfile *dac);

// Normalized 7-bit level -> physical DAC code. Init-time only.
uint32_t compositeLevelToCode(const CompositeLevelMap &map, int32_t level);

// Classify a field line and, for active lines, report the source row.
CompositeLineType compositeClassifyLine(const CompositeTiming *timing,
                                        uint32_t line, uint32_t *activeY);

void compositeWriterInit(CompositeSampleWriter *w, const CompositeEncoder *e,
                         void *dst);

// Emit exactly timing->samplesPerLine samples for one line, in the transfer
// format of the sink the encoder was initialized for. `px` is ignored for
// every type except ACTIVE, where it must hold hActivePixels RGB565 values.
inline void compositeEmitLine(CompositeSampleWriter *w,
                              const CompositeEncoder *e, CompositeLineType type,
                              const uint16_t *px) {
  e->emitLine(w, e, type, px);
}

// The two concrete formats. Callers should use compositeEmitLine() instead;
// these are what compositeEncoderInit() installs.
void compositeEmitLinePacked(CompositeSampleWriter *w,
                             const CompositeEncoder *e, CompositeLineType type,
                             const uint16_t *px);
void compositeEmitLinePwm(CompositeSampleWriter *w, const CompositeEncoder *e,
                          CompositeLineType type, const uint16_t *px);

}  // namespace lcdtap::pico2
