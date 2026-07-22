#include "lcdtap/pico2/composite_encode.hpp"

// The per-line generator is force-inlined into the two public emit entry
// points, which are placed in SRAM via LCDTAP_CVBS_HOT. Inlining is what makes
// that placement effective: it pulls the policy put()/run() and the burst /
// active helpers into the sectioned functions instead of leaving them in
// flash.
#define LCDTAP_CVBS_INLINE inline __attribute__((always_inline))

namespace lcdtap::pico2 {

namespace {

uint32_t gcd(uint32_t a, uint32_t b) {
  while (b != 0u) {
    const uint32_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

int32_t clampLevel(int32_t v) {
  if (v < 0) return 0;
  if (v > static_cast<int32_t>(COMPOSITE_LEVEL_MAX)) {
    return static_cast<int32_t>(COMPOSITE_LEVEL_MAX);
  }
  return v;
}

// RGB565 -> RGB332. Five ALU ops; cheaper than a 64 KB lookup on Cortex-M33.
LCDTAP_CVBS_INLINE uint32_t rgb565ToIndex(uint32_t v) {
  return ((v >> 8) & 0xE0u) | ((v >> 6) & 0x1Cu) | ((v >> 3) & 0x03u);
}

uint32_t lcm(uint32_t a, uint32_t b) { return a / gcd(a, b) * b; }

// --- sink cursors ----------------------------------------------------------
//
// All writer state lives in cursor locals for the duration of a line, loaded
// from CompositeSampleWriter at entry and stored back at exit. Going through
// the struct pointer inside the sample loops costs a reload of every member
// after each buffer store: the stores and the members are both uint32_t, so
// the compiler must assume aliasing. Measured on the previous build that was
// ~6 loads + ~5 stores of state per sample -- several times the useful work.
//
// The cursor is a template parameter of emitLineT so the line structure is
// written once; everything per-sample is specialized per sink, with the
// packing geometry as compile-time constants. There are exactly two sinks
// (R-2R, PWM) and no plans for more, so the duplication below is bounded and
// deliberate -- this is the trade of abstraction for cycles that the PAL
// line budget demands.

// R-2R: four 7-bit samples bit-packed into each 32-bit word.
struct R2rCursor {
  static constexpr uint32_t BITS = 7u;
  static constexpr uint32_t FLUSH = 28u;
  static constexpr uint32_t SPW = 4u;  // samples per word
  // A 7-bit code replicated into all four sample lanes with one multiply.
  static constexpr uint32_t LANES = (1u << 21) | (1u << 14) | (1u << 7) | 1u;

  uint32_t *dst;
  uint32_t acc;
  uint32_t nbits;  // 0, 7, 14 or 21
  uint32_t phase;  // absolute sample index; low two bits = subcarrier phase

  LCDTAP_CVBS_INLINE void load(const CompositeSampleWriter *w) {
    dst = w->dst.w;
    acc = w->acc;
    nbits = w->nbits;
    phase = w->sampleIndex;
  }
  LCDTAP_CVBS_INLINE void store(CompositeSampleWriter *w) const {
    w->dst.w = dst;
    w->acc = acc;
    w->nbits = nbits;
    w->sampleIndex = phase;
  }

  LCDTAP_CVBS_INLINE void put(uint32_t code) {
    acc |= code << nbits;
    nbits += BITS;
    if (nbits >= FLUSH) {
      *dst++ = acc;
      acc = 0u;
      nbits = 0u;
    }
    phase++;
  }

  LCDTAP_CVBS_INLINE void run(uint32_t code, uint32_t n) {
    while (n != 0u && nbits != 0u) {
      put(code);
      --n;
    }
    if (n >= SPW) {
      const uint32_t packed = code * LANES;
      const uint32_t words = n / SPW;
      phase += words * SPW;
      n -= words * SPW;
      for (uint32_t i = 0u; i < words; ++i) *dst++ = packed;
    }
    while (n != 0u) {
      put(code);
      --n;
    }
  }

  LCDTAP_CVBS_INLINE void emitBurst(const CompositeEncoder *e, uint32_t vSwitch,
                                    uint32_t n) {
    for (uint32_t i = 0u; i < n; ++i) put(e->burstCode[vSwitch][phase & 3u]);
  }

  // Active window. Each LUT entry already holds a pixel's two samples as a
  // packed 14-bit pair (low half: pixel starting at this phase; bits 16..29:
  // the same pixel 180 degrees later), so splice 14 bits at a time instead of
  // unpacking to four 7-bit puts. Two pixels are 28 bits -- exactly one word
  // -- so nbits returns to its entry value after every pixel pair, and the
  // packing alignment can be hoisted out of the loop: four branch-free inner
  // loops, one per entry alignment. nPx is even by the timing tables.
  LCDTAP_CVBS_INLINE void emitActive(const CompositeEncoder *e,
                                     uint32_t vSwitch, const uint16_t *px,
                                     uint32_t nPx) {
    const uint32_t *lutSet =
        e->lut + (((phase & 3u) | (vSwitch << 2)) * COMPOSITE_LUT_COLORS);
    switch (nbits) {
      case 0u:
        for (uint32_t i = 0u; i < nPx; i += 2u) {
          const uint32_t pA = lutSet[rgb565ToIndex(px[i])] & 0x3FFFu;
          const uint32_t pB = lutSet[rgb565ToIndex(px[i + 1u])] >> 16;
          *dst++ = pA | (pB << 14);
        }
        break;
      case 7u:
        for (uint32_t i = 0u; i < nPx; i += 2u) {
          const uint32_t pA = lutSet[rgb565ToIndex(px[i])] & 0x3FFFu;
          const uint32_t pB = lutSet[rgb565ToIndex(px[i + 1u])] >> 16;
          *dst++ = acc | (pA << 7) | ((pB & 0x7Fu) << 21);
          acc = pB >> 7;
        }
        break;
      case 14u:
        for (uint32_t i = 0u; i < nPx; i += 2u) {
          const uint32_t pA = lutSet[rgb565ToIndex(px[i])] & 0x3FFFu;
          const uint32_t pB = lutSet[rgb565ToIndex(px[i + 1u])] >> 16;
          *dst++ = acc | (pA << 14);
          acc = pB;
        }
        break;
      default:  // 21
        for (uint32_t i = 0u; i < nPx; i += 2u) {
          const uint32_t pA = lutSet[rgb565ToIndex(px[i])] & 0x3FFFu;
          const uint32_t pB = lutSet[rgb565ToIndex(px[i + 1u])] >> 16;
          *dst++ = acc | ((pA & 0x7Fu) << 21);
          acc = (pA >> 7) | (pB << 7);
        }
        break;
    }
    phase += nPx * 2u;
  }
};

// PWM: one 16-bit counter-compare value per sample, no packing state.
struct PwmCursor {
  uint16_t *dst;
  uint32_t phase;

  LCDTAP_CVBS_INLINE void load(const CompositeSampleWriter *w) {
    dst = w->dst.h;
    phase = w->sampleIndex;
  }
  LCDTAP_CVBS_INLINE void store(CompositeSampleWriter *w) const {
    w->dst.h = dst;
    w->sampleIndex = phase;
    // acc/nbits stay 0 for this sink; compositeWriterInit set them.
  }

  LCDTAP_CVBS_INLINE void put(uint32_t code) {
    *dst++ = static_cast<uint16_t>(code);
    phase++;
  }

  LCDTAP_CVBS_INLINE void run(uint32_t code, uint32_t n) {
    // Write pairs as 32-bit stores. samplesPerLine is odd on PAL, so the
    // destination alternates between aligned and unaligned per line -- emit a
    // leading single sample when needed rather than assuming either.
    if (n != 0u && ((reinterpret_cast<uintptr_t>(dst) & 3u) != 0u)) {
      put(code);
      --n;
    }
    const uint32_t pairs = n / 2u;
    if (pairs != 0u) {
      const uint32_t both = code | (code << 16);
      uint32_t *p = reinterpret_cast<uint32_t *>(dst);
      for (uint32_t i = 0u; i < pairs; ++i) *p++ = both;
      dst = reinterpret_cast<uint16_t *>(p);
      phase += pairs * 2u;
      n -= pairs * 2u;
    }
    if (n != 0u) put(code);
  }

  LCDTAP_CVBS_INLINE void emitBurst(const CompositeEncoder *e, uint32_t vSwitch,
                                    uint32_t n) {
    for (uint32_t i = 0u; i < n; ++i) put(e->burstCode[vSwitch][phase & 3u]);
  }

  // Active window. The PWM LUT stores ready-made 32-bit words (two 16-bit
  // samples each): one load and one store per pixel, no unpacking. The word
  // alignment of dst is constant across the window, so it is resolved once.
  LCDTAP_CVBS_INLINE void emitActive(const CompositeEncoder *e,
                                     uint32_t vSwitch, const uint16_t *px,
                                     uint32_t nPx) {
    const uint32_t *setA = e->lut + (((phase & 3u) | (vSwitch << 2)) *
                                     (COMPOSITE_LUT_COLORS * 2u));
    const uint32_t *setB = setA + COMPOSITE_LUT_COLORS;
    if ((reinterpret_cast<uintptr_t>(dst) & 3u) == 0u) {
      uint32_t *p = reinterpret_cast<uint32_t *>(dst);
      for (uint32_t i = 0u; i < nPx; i += 2u) {
        *p++ = setA[rgb565ToIndex(px[i])];
        *p++ = setB[rgb565ToIndex(px[i + 1u])];
      }
      dst = reinterpret_cast<uint16_t *>(p);
    } else {
      for (uint32_t i = 0u; i < nPx; i += 2u) {
        const uint32_t a = setA[rgb565ToIndex(px[i])];
        const uint32_t b = setB[rgb565ToIndex(px[i + 1u])];
        dst[0] = static_cast<uint16_t>(a);
        dst[1] = static_cast<uint16_t>(a >> 16);
        dst[2] = static_cast<uint16_t>(b);
        dst[3] = static_cast<uint16_t>(b >> 16);
        dst += 4;
      }
    }
    phase += nPx * 2u;
  }
};

// Blank from the current position up to `target` samples into the line.
template <class C>
LCDTAP_CVBS_INLINE void cwBlankTo(C &c, const CompositeEncoder *e,
                                  uint32_t lineStart, uint32_t target) {
  const uint32_t at = c.phase - lineStart;
  if (target > at) c.run(e->codeBlank, target - at);
}

uint32_t samplesPerTransferOf(const CompositeDacProfile *dac) {
  return dac->kind == CompositeDacKind::PWM ? 1u : 32u / dac->bits;
}

uint32_t bytesPerTransferOf(const CompositeDacProfile *dac) {
  return dac->kind == CompositeDacKind::PWM ? 2u : 4u;
}

}  // namespace

uint32_t compositeLinesPerSlot(const CompositeTiming *timing,
                               const CompositeDacProfile *dac) {
  const uint32_t spt = samplesPerTransferOf(dac);
  const uint32_t alignLines = spt / gcd(spt, timing->samplesPerLine);
  return lcm(alignLines, COMPOSITE_MIN_LINES_PER_SLOT);
}

uint32_t compositeTransfersPerSlot(const CompositeTiming *timing,
                                   const CompositeDacProfile *dac) {
  return compositeLinesPerSlot(timing, dac) * timing->samplesPerLine /
         samplesPerTransferOf(dac);
}

uint32_t compositeBytesPerSlot(const CompositeTiming *timing,
                               const CompositeDacProfile *dac) {
  return compositeTransfersPerSlot(timing, dac) * bytesPerTransferOf(dac);
}

uint32_t compositeLevelToCode(const CompositeLevelMap &map, int32_t level) {
  level = clampLevel(level);
  uint32_t code =
      (static_cast<uint32_t>(level) * map.codeWhite + map.levelWhite / 2u) /
      map.levelWhite;
  if (code > map.codeMax) code = map.codeMax;
  return code;
}

namespace {

// Four subcarrier phase sets; PAL colour doubles them for the alternating
// V axis.
uint32_t lutSetsOf(const CompositeTiming *timing) {
  return timing->palSwitch && timing->colorEnabled ? COMPOSITE_NUM_PHASES * 2u
                                                   : COMPOSITE_NUM_PHASES;
}

}  // namespace

uint32_t compositeLutWords(const CompositeTiming *timing,
                           const CompositeDacProfile *dac) {
  // The PWM layout stores two ready-made words per colour (see the lut
  // comment in composite_encode.hpp); the R-2R layout packs one.
  const uint32_t wordsPerColor = dac->kind == CompositeDacKind::PWM ? 2u : 1u;
  return lutSetsOf(timing) * COMPOSITE_LUT_COLORS * wordsPerColor;
}

bool compositeEncoderInit(CompositeEncoder *e, const CompositeTiming *timing,
                          const CompositeDacProfile *dac,
                          uint32_t *lutStorage) {
  if (timing == nullptr || dac == nullptr || lutStorage == nullptr)
    return false;
  if ((timing->hActivePixels & 1u) != 0u) return false;
  if (timing->hActiveSamples != timing->hActivePixels * 2u) return false;

  e->timing = timing;
  e->dac = dac;
  e->samplesPerTransfer = samplesPerTransferOf(dac);
  e->bytesPerTransfer = bytesPerTransferOf(dac);
  e->linesPerSlot = compositeLinesPerSlot(timing, dac);
  e->transfersPerSlot = compositeTransfersPerSlot(timing, dac);
  e->bytesPerSlot = compositeBytesPerSlot(timing, dac);
  if (e->linesPerSlot > COMPOSITE_MAX_LINES_PER_SLOT) return false;

  if (dac->kind == CompositeDacKind::PWM) {
    e->bitsPerSample = 0u;
    e->flushBits = 0u;
    e->emitLine = compositeEmitLinePwm;
  } else {
    // R2rCursor hardcodes the 7-bit packing geometry; the only ladder is the
    // 7-bit one and there are no plans for another, so reject anything else
    // rather than carrying runtime-width packing on the hot path.
    if (dac->bits != COMPOSITE_LEVEL_BITS) return false;
    e->bitsPerSample = dac->bits;
    e->flushBits = e->samplesPerTransfer * dac->bits;
    e->emitLine = compositeEmitLinePacked;
  }

  const CompositeLevelMap map = compositeLevelMap(timing, dac);
  e->codeMax = map.codeMax;
  // The LUT packs samples into 7-bit fields, so no physical code may exceed
  // that. Holds for both sinks (127 and 22/17), but assert rather than assume.
  if (map.codeMax > COMPOSITE_LEVEL_MAX) return false;

  e->codeSyncTip = compositeLevelToCode(map, timing->lvlSyncTip);
  e->codeBlank = compositeLevelToCode(map, timing->lvlBlank);
  e->codeWhite = compositeLevelToCode(map, timing->lvlWhite);

  // Burst phases on a 4x fsc grid, where sample k carries {+U, -V, -U, +V}.
  //
  // NTSC: burst sits on the -U axis, so on this grid it collapses to the
  // blanking level +/- amp on phases 0 and 2 and plain blanking on 1 and 3.
  // Both polarity rows are filled identically -- NTSC never switches.
  //
  // PAL: the burst swings +/-45 degrees line to line, at -U+V and -U-V (135
  // and 225 degrees). Each component is amp/sqrt(2) ~= amp * 181/256, so no
  // single phase reaches +/-amp. Polarity 0 is the +V line, polarity 1 the -V
  // line; the pairing with the LUT V sign is a convention pinned by
  // testChromaPhase() and a real receiver, exactly as the NTSC hue sign was.
  const int32_t blank = timing->lvlBlank;
  const int32_t amp = timing->burstAmplitude;
  const bool burst = timing->colorEnabled && timing->hBurstCycles != 0u;
  if (!burst) {
    for (uint32_t pol = 0u; pol < 2u; ++pol) {
      for (uint32_t k = 0u; k < COMPOSITE_NUM_PHASES; ++k) {
        e->burstCode[pol][k] = compositeLevelToCode(map, blank);
      }
    }
  } else if (timing->palSwitch) {
    const int32_t b = (amp * 181 + 128) >> 8;  // amp / sqrt(2), rounded
    // Phase order {+U, -V, -U, +V}: -U+V -> {-b, -b, +b, +b}, -U-V ->
    // {-b, +b, +b, -b}.
    const int32_t lo = blank - b;
    const int32_t hi = blank + b;
    const int32_t polA[COMPOSITE_NUM_PHASES] = {lo, lo, hi, hi};
    const int32_t polB[COMPOSITE_NUM_PHASES] = {lo, hi, hi, lo};
    for (uint32_t k = 0u; k < COMPOSITE_NUM_PHASES; ++k) {
      e->burstCode[0][k] = compositeLevelToCode(map, polA[k]);
      e->burstCode[1][k] = compositeLevelToCode(map, polB[k]);
    }
  } else {
    const uint32_t neg = compositeLevelToCode(map, blank - amp);
    const uint32_t mid = compositeLevelToCode(map, blank);
    const uint32_t pos = compositeLevelToCode(map, blank + amp);
    for (uint32_t pol = 0u; pol < 2u; ++pol) {
      e->burstCode[pol][0] = neg;
      e->burstCode[pol][1] = mid;
      e->burstCode[pol][2] = pos;
      e->burstCode[pol][3] = mid;
    }
  }

  e->lut = lutStorage;
  e->lutSets = lutSetsOf(timing);

  const int32_t lumaSpan =
      static_cast<int32_t>(timing->lvlWhite) - timing->lvlBlack;
  const int32_t chromaGain = timing->colorEnabled ? timing->chromaGain : 0;
  const bool pwmLut = dac->kind == CompositeDacKind::PWM;

  // Sets 0..3 are the four subcarrier phases. For PAL colour, sets 4..7 repeat
  // them with the V axis inverted -- the line-alternating switch that
  // emitActive() selects between via vSwitch. NTSC has only the lower four.
  for (uint32_t set = 0u; set < e->lutSets; ++set) {
    uint32_t *dst = e->lut + set * COMPOSITE_LUT_COLORS * (pwmLut ? 2u : 1u);
    const uint32_t phase0 = set & 3u;
    const int32_t vSign = (set >= COMPOSITE_NUM_PHASES) ? -1 : 1;
    for (uint32_t idx = 0u; idx < COMPOSITE_LUT_COLORS; ++idx) {
      const int32_t r8 = static_cast<int32_t>(((idx >> 5) & 7u) * 255u / 7u);
      const int32_t g8 = static_cast<int32_t>(((idx >> 2) & 7u) * 255u / 7u);
      const int32_t b8 = static_cast<int32_t>((idx & 3u) * 255u / 3u);

      const int32_t y = (77 * r8 + 150 * g8 + 29 * b8) >> 8;
      const int32_t lvl = timing->lvlBlack + (y * lumaSpan) / 255;
      const int32_t u = ((b8 - y) * 126) >> 8;  // 0.492 * (B - Y)
      const int32_t v = ((r8 - y) * 225) >> 8;  // 0.879 * (R - Y)

      // Four consecutive samples, one per subcarrier phase.
      //
      // NTSC modulates as C = U*sin(wt) + V*cos(wt). With the burst on -U
      // (phases 0 and 2 here), that places +V a quarter cycle EARLIER than
      // +U, not later -- hence -v on phase 1 and +v on phase 3. Getting this
      // backwards mirrors every hue about the U axis while leaving luma
      // untouched, which is exactly what the first hardware test showed:
      // skin and brown came out green, green came out orange, cyan came out
      // purple. testChromaPhase() demodulates the colour bars against the
      // burst and pins this down.
      //
      // A 90 degree absolute offset remains against the textbook form, but
      // the burst carries the same offset, so the receiver -- which has no
      // reference other than the burst -- cannot see it.
      //
      // vSign negates the V component for the upper (PAL inverted-V) sets.
      uint32_t s[COMPOSITE_NUM_PHASES];
      for (uint32_t k = 0u; k < COMPOSITE_NUM_PHASES; ++k) {
        int32_t c;
        switch ((phase0 + k) & 3u) {
          case 0: c = u; break;
          case 1: c = -vSign * v; break;
          case 2: c = -u; break;
          default: c = vSign * v; break;
        }
        const int32_t off = (c * lumaSpan * chromaGain) / (255 * 256);
        s[k] = compositeLevelToCode(map, clampLevel(lvl + off));
      }
      // Sink-specific layout; see the lut comment in composite_encode.hpp.
      if (pwmLut) {
        // Two ready-to-store words: pixel starting at `phase0`, then the
        // 180-degree variant in a parallel block.
        dst[idx] = s[0] | (s[1] << 16);
        dst[COMPOSITE_LUT_COLORS + idx] = s[2] | (s[3] << 16);
      } else {
        // Low half: pixel starting at `phase0`. High half: 180 degrees later.
        dst[idx] = (s[0] | (s[1] << 7)) | ((s[2] | (s[3] << 7)) << 16);
      }
    }
  }
  return true;
}

CompositeLineType LCDTAP_CVBS_HOT(compositeClassifyLine)(
    const CompositeTiming *timing, uint32_t line, uint32_t *activeY) {
  const uint32_t preEqEnd = timing->vPreEqLines;
  const uint32_t serrEnd = preEqEnd + timing->vSerrLines;
  const uint32_t postEqEnd = serrEnd + timing->vPostEqLines;
  const uint32_t activeStart = timing->vTotalLines - timing->vActiveLines;

  if (line < preEqEnd) return CompositeLineType::EQ;
  if (line < serrEnd) return CompositeLineType::SERR;
  if (line < postEqEnd) return CompositeLineType::EQ;
  if (line < activeStart) {
    return line < timing->vBurstBlankEnd ? CompositeLineType::BLANK_NOBURST
                                         : CompositeLineType::BLANK_BURST;
  }
  *activeY = line - activeStart;
  return CompositeLineType::ACTIVE;
}

void LCDTAP_CVBS_HOT(compositeWriterInit)(CompositeSampleWriter *w,
                                          const CompositeEncoder *e, void *dst,
                                          uint32_t firstLine) {
  w->dst.w = static_cast<uint32_t *>(dst);
  w->acc = 0u;
  w->nbits = 0u;
  // Seed with the slot's absolute sample offset so sampleIndex & 3 is the true
  // subcarrier phase. Only the low two bits matter, but the full count keeps
  // the "samples emitted" bookkeeping meaningful.
  w->sampleIndex = firstLine * e->timing->samplesPerLine;
}

namespace {

// The line structure, written once and instantiated per sink cursor. All
// writer state stays in the cursor (registers) from here to the end of the
// line; see the cursor block above for why.
template <class C>
LCDTAP_CVBS_INLINE void emitLineT(CompositeSampleWriter *w,
                                  const CompositeEncoder *e,
                                  CompositeLineType type, uint32_t line,
                                  const uint16_t *px) {
  const CompositeTiming *t = e->timing;
  C c;
  c.load(w);
  const uint32_t lineStart = c.phase;
  // PAL alternates the V axis (and the burst swing) every line; NTSC does not.
  const uint32_t vSwitch = (t->palSwitch && t->colorEnabled) ? (line & 1u) : 0u;

  switch (type) {
    case CompositeLineType::EQ: {
      // Two narrow sync pulses, one per half line.
      c.run(e->codeSyncTip, t->eqPulseWidth);
      c.run(e->codeBlank, t->halfLineSamplesA - t->eqPulseWidth);
      c.run(e->codeSyncTip, t->eqPulseWidth);
      c.run(e->codeBlank, t->halfLineSamplesB - t->eqPulseWidth);
      break;
    }
    case CompositeLineType::SERR: {
      // Two broad pulses: sync tip for most of each half line.
      c.run(e->codeSyncTip, t->halfLineSamplesA - t->serrPulseWidth);
      c.run(e->codeBlank, t->serrPulseWidth);
      c.run(e->codeSyncTip, t->halfLineSamplesB - t->serrPulseWidth);
      c.run(e->codeBlank, t->serrPulseWidth);
      break;
    }
    case CompositeLineType::BLANK_NOBURST: {
      c.run(e->codeSyncTip, t->hSyncWidth);
      c.run(e->codeBlank, t->samplesPerLine - t->hSyncWidth);
      break;
    }
    case CompositeLineType::BLANK_BURST:
    case CompositeLineType::ACTIVE: {
      // Both share the same prefix; ACTIVE splices video into the middle.
      c.run(e->codeSyncTip, t->hSyncWidth);
      if (t->colorEnabled && t->hBurstCycles != 0u) {
        cwBlankTo(c, e, lineStart, t->hBurstStart);
        c.emitBurst(e, vSwitch, t->hBurstCycles * COMPOSITE_NUM_PHASES);
      }
      if (type == CompositeLineType::ACTIVE) {
        cwBlankTo(c, e, lineStart, t->hActiveStart);
        c.emitActive(e, vSwitch, px, t->hActivePixels);
      }
      cwBlankTo(c, e, lineStart, t->samplesPerLine);
      break;
    }
  }
  c.store(w);
}

}  // namespace

void LCDTAP_CVBS_HOT(compositeEmitLinePacked)(CompositeSampleWriter *w,
                                              const CompositeEncoder *e,
                                              CompositeLineType type,
                                              uint32_t line,
                                              const uint16_t *px) {
  emitLineT<R2rCursor>(w, e, type, line, px);
}

void LCDTAP_CVBS_HOT(compositeEmitLinePwm)(CompositeSampleWriter *w,
                                           const CompositeEncoder *e,
                                           CompositeLineType type,
                                           uint32_t line, const uint16_t *px) {
  emitLineT<PwmCursor>(w, e, type, line, px);
}

}  // namespace lcdtap::pico2
