#pragma once

// Composite video sample encoding. This layer has no MCU dependencies so that
// a whole field can be generated and verified on a host machine before any of
// it is run against an oscilloscope.
//
// Sample packing
// --------------
// The PIO state machine shifts right out of the OSR with an autopull
// threshold of flushBits, so sample 0 occupies the least significant bits of
// each word:
//
//   word = s0 | s1 << bits | s2 << (2*bits) | ...
//
// Only floor(32 / bits) samples fit per word; the remaining high bits are
// discarded by the autopull and are never shifted out.
//
// Line grouping
// -------------
// samplesPerLine is not generally a multiple of samplesPerWord (NTSC 910,
// PAL 1135), so a single line does not occupy a whole number of words. The
// DMA unit is therefore a *group* of linesPerSlot lines, chosen so that the
// group is word-aligned:
//
//   linesPerSlot = samplesPerWord / gcd(samplesPerWord, samplesPerLine)
//
// The writer runs continuously from the start of a group to its end, so no
// code below ever has to reason about alignment.

#include <cstdint>

#include "lcdtap/pico2/composite_timing.hpp"

namespace lcdtap::pico2 {

// Upper bound on linesPerSlot across all supported timing/DAC combinations
// (worst case is PAL with the 7-bit DAC: gcd(4, 1135) = 1).
static constexpr uint32_t COMPOSITE_MAX_LINES_PER_SLOT = 4u;

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
struct CompositeEncoder {
  const CompositeTiming *timing;
  const CompositeDacProfile *dac;

  // Packing geometry
  uint32_t bitsPerSample;
  uint32_t samplesPerWord;
  uint32_t flushBits;
  uint32_t linesPerSlot;
  uint32_t wordsPerSlot;

  // Physical DAC codes for the constant levels
  uint32_t codeSyncTip;
  uint32_t codeBlank;

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

// Sequential sample packer. `sampleIndex` counts from the start of the group
// and drives subcarrier phase, so nothing needs to be word-aligned.
struct CompositeSampleWriter {
  uint32_t *dst;
  uint32_t acc;
  uint32_t nbits;
  uint32_t sampleIndex;
  uint32_t bitsPerSample;
  uint32_t samplesPerWord;
  uint32_t flushBits;
};

// Compute the derived geometry, the physical level codes and the chroma LUT.
// `lutStorage` must hold at least compositeLutWords(timing) uint32_t.
// Returns false if the combination is unsupported.
bool compositeEncoderInit(CompositeEncoder *e, const CompositeTiming *timing,
                          const CompositeDacProfile *dac, uint32_t *lutStorage);

// Number of uint32_t the caller must provide for the LUT.
uint32_t compositeLutWords(const CompositeTiming *timing);

// Words occupied by one line group, for the given timing and DAC profile.
uint32_t compositeWordsPerSlot(const CompositeTiming *timing,
                               const CompositeDacProfile *dac);
uint32_t compositeLinesPerSlot(const CompositeTiming *timing,
                               const CompositeDacProfile *dac);

// Classify a field line and, for active lines, report the source row.
CompositeLineType compositeClassifyLine(const CompositeTiming *timing,
                                        uint32_t line, uint32_t *activeY);

void compositeWriterInit(CompositeSampleWriter *w, const CompositeEncoder *e,
                         uint32_t *dst);

// Emit exactly timing->samplesPerLine samples for one line. `px` is ignored
// for every type except ACTIVE, where it must hold hActivePixels RGB565
// values.
void compositeEmitLine(CompositeSampleWriter *w, const CompositeEncoder *e,
                       CompositeLineType type, const uint16_t *px);

}  // namespace lcdtap::pico2
