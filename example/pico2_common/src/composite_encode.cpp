#include "lcdtap/pico2/composite_encode.hpp"

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
inline uint32_t rgb565ToIndex(uint32_t v) {
  return ((v >> 8) & 0xE0u) | ((v >> 6) & 0x1Cu) | ((v >> 3) & 0x03u);
}

uint32_t lcm(uint32_t a, uint32_t b) { return a / gcd(a, b) * b; }

// --- writer policies ------------------------------------------------------
//
// Both expose put() and run(); everything above them is written once. The
// policy is a template parameter rather than a runtime branch so that run()
// keeps its whole-transfer fast path: an EQ, SERR or BLANK line is 100%
// constant levels, and those lines are 22 of every 262 on NTSC.

// R-2R: bit-pack several samples into each 32-bit word.
struct PackedPolicy {
  static inline void put(CompositeSampleWriter *w, uint32_t code) {
    w->acc |= code << w->nbits;
    w->nbits += w->bitsPerSample;
    if (w->nbits >= w->flushBits) {
      *w->dst.w++ = w->acc;
      w->acc = 0u;
      w->nbits = 0u;
    }
    w->sampleIndex++;
  }

  static void run(CompositeSampleWriter *w, uint32_t code, uint32_t n) {
    while (n != 0u && w->nbits != 0u) {
      put(w, code);
      --n;
    }
    if (n >= w->samplesPerTransfer) {
      uint32_t packed = 0u;
      for (uint32_t i = 0u; i < w->samplesPerTransfer; ++i) {
        packed |= code << (i * w->bitsPerSample);
      }
      const uint32_t words = n / w->samplesPerTransfer;
      for (uint32_t i = 0u; i < words; ++i) *w->dst.w++ = packed;
      const uint32_t done = words * w->samplesPerTransfer;
      w->sampleIndex += done;
      n -= done;
    }
    while (n != 0u) {
      put(w, code);
      --n;
    }
  }
};

// PWM: one 16-bit counter-compare value per sample.
struct DirectPolicy {
  static inline void put(CompositeSampleWriter *w, uint32_t code) {
    *w->dst.h++ = static_cast<uint16_t>(code);
    w->sampleIndex++;
  }

  static void run(CompositeSampleWriter *w, uint32_t code, uint32_t n) {
    // Write pairs as 32-bit stores. samplesPerLine is odd on PAL, so the
    // destination alternates between aligned and unaligned per line -- emit a
    // leading single sample when needed rather than assuming either.
    if (n != 0u && ((reinterpret_cast<uintptr_t>(w->dst.h) & 3u) != 0u)) {
      put(w, code);
      --n;
    }
    const uint32_t pairs = n / 2u;
    if (pairs != 0u) {
      const uint32_t both = code | (code << 16);
      uint32_t *p = reinterpret_cast<uint32_t *>(w->dst.h);
      for (uint32_t i = 0u; i < pairs; ++i) *p++ = both;
      w->dst.h = reinterpret_cast<uint16_t *>(p);
      w->sampleIndex += pairs * 2u;
      n -= pairs * 2u;
    }
    while (n != 0u) {
      put(w, code);
      --n;
    }
  }
};

// Emit the colour burst: `n` samples modulated at the subcarrier frequency,
// taking the phase from the absolute sample index.
template <class P>
void cwEmitBurst(CompositeSampleWriter *w, const CompositeEncoder *e,
                 uint32_t n) {
  for (uint32_t i = 0u; i < n; ++i) {
    P::put(w, e->burstCode[w->sampleIndex & 3u]);
  }
}

// Emit the active window. Two samples per pixel means consecutive pixels
// start 180 degrees apart, so they alternate between the two halves of a LUT
// entry. nPx is guaranteed even by the timing tables.
template <class P>
void cwEmitActive(CompositeSampleWriter *w, const CompositeEncoder *e,
                  const uint16_t *px, uint32_t nPx) {
  const uint32_t *lutSet =
      e->lut + (w->sampleIndex & 3u) * COMPOSITE_LUT_COLORS;
  for (uint32_t i = 0u; i < nPx; i += 2u) {
    const uint32_t a = lutSet[rgb565ToIndex(px[i])];
    const uint32_t b = lutSet[rgb565ToIndex(px[i + 1u])];
    P::put(w, a & 0x7Fu);
    P::put(w, (a >> 7) & 0x7Fu);
    P::put(w, (b >> 16) & 0x7Fu);
    P::put(w, (b >> 23) & 0x7Fu);
  }
}

// Blank from the current position up to `target` samples into the line.
template <class P>
inline void cwBlankTo(CompositeSampleWriter *w, const CompositeEncoder *e,
                      uint32_t lineStart, uint32_t target) {
  const uint32_t at = w->sampleIndex - lineStart;
  if (target > at) P::run(w, e->codeBlank, target - at);
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

uint32_t compositeLutWords(const CompositeTiming *timing) {
  // Phase 1 needs the four subcarrier phases. PAL colour will double this for
  // the alternating V axis.
  const uint32_t sets = timing->palSwitch && timing->colorEnabled
                            ? COMPOSITE_NUM_PHASES * 2u
                            : COMPOSITE_NUM_PHASES;
  return sets * COMPOSITE_LUT_COLORS;
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
    if (dac->bits == 0u || dac->bits > COMPOSITE_LEVEL_BITS) return false;
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

  // Burst sits on the -U axis. On a 4x fsc grid the quadrature mixer
  // degenerates to cos = {+1, 0, -1, 0}, sin = {0, +1, 0, -1}, so a 180
  // degree burst is simply the blanking level +/- the burst amplitude.
  const int32_t blank = timing->lvlBlank;
  const int32_t amp = timing->burstAmplitude;
  const bool burst = timing->colorEnabled && timing->hBurstCycles != 0u;
  e->burstCode[0] = compositeLevelToCode(map, burst ? blank - amp : blank);
  e->burstCode[1] = compositeLevelToCode(map, blank);
  e->burstCode[2] = compositeLevelToCode(map, burst ? blank + amp : blank);
  e->burstCode[3] = compositeLevelToCode(map, blank);

  e->lut = lutStorage;
  e->lutSets = compositeLutWords(timing) / COMPOSITE_LUT_COLORS;

  const int32_t lumaSpan =
      static_cast<int32_t>(timing->lvlWhite) - timing->lvlBlack;
  const int32_t chromaGain = timing->colorEnabled ? timing->chromaGain : 0;

  for (uint32_t set = 0u; set < COMPOSITE_NUM_PHASES; ++set) {
    uint32_t *dst = e->lut + set * COMPOSITE_LUT_COLORS;
    for (uint32_t idx = 0u; idx < COMPOSITE_LUT_COLORS; ++idx) {
      const int32_t r8 = static_cast<int32_t>(((idx >> 5) & 7u) * 255u / 7u);
      const int32_t g8 = static_cast<int32_t>(((idx >> 2) & 7u) * 255u / 7u);
      const int32_t b8 = static_cast<int32_t>((idx & 3u) * 255u / 3u);

      const int32_t y = (77 * r8 + 150 * g8 + 29 * b8) >> 8;
      const int32_t lvl = timing->lvlBlack + (y * lumaSpan) / 255;
      const int32_t u = ((b8 - y) * 126) >> 8;  // 0.492 * (B - Y)
      const int32_t v = ((r8 - y) * 225) >> 8;  // 0.879 * (R - Y)

      // Four consecutive samples, one per subcarrier phase.
      uint32_t s[COMPOSITE_NUM_PHASES];
      for (uint32_t k = 0u; k < COMPOSITE_NUM_PHASES; ++k) {
        int32_t c;
        switch ((set + k) & 3u) {
          case 0: c = u; break;
          case 1: c = v; break;
          case 2: c = -u; break;
          default: c = -v; break;
        }
        const int32_t off = (c * lumaSpan * chromaGain) / (255 * 256);
        s[k] = compositeLevelToCode(map, clampLevel(lvl + off));
      }
      // Low half: pixel starting at `set`. High half: 180 degrees later.
      dst[idx] = (s[0] | (s[1] << 7)) | ((s[2] | (s[3] << 7)) << 16);
    }
  }
  return true;
}

CompositeLineType compositeClassifyLine(const CompositeTiming *timing,
                                        uint32_t line, uint32_t *activeY) {
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

void compositeWriterInit(CompositeSampleWriter *w, const CompositeEncoder *e,
                         void *dst) {
  w->dst.w = static_cast<uint32_t *>(dst);
  w->acc = 0u;
  w->nbits = 0u;
  w->sampleIndex = 0u;
  w->bitsPerSample = e->bitsPerSample;
  w->samplesPerTransfer = e->samplesPerTransfer;
  w->flushBits = e->flushBits;
}

namespace {

// The line structure, written once and instantiated per writer policy.
template <class P>
void emitLineT(CompositeSampleWriter *w, const CompositeEncoder *e,
               CompositeLineType type, const uint16_t *px) {
  const CompositeTiming *t = e->timing;
  const uint32_t lineStart = w->sampleIndex;

  switch (type) {
    case CompositeLineType::EQ: {
      // Two narrow sync pulses, one per half line.
      P::run(w, e->codeSyncTip, t->eqPulseWidth);
      P::run(w, e->codeBlank, t->halfLineSamplesA - t->eqPulseWidth);
      P::run(w, e->codeSyncTip, t->eqPulseWidth);
      P::run(w, e->codeBlank, t->halfLineSamplesB - t->eqPulseWidth);
      break;
    }
    case CompositeLineType::SERR: {
      // Two broad pulses: sync tip for most of each half line.
      P::run(w, e->codeSyncTip, t->halfLineSamplesA - t->serrPulseWidth);
      P::run(w, e->codeBlank, t->serrPulseWidth);
      P::run(w, e->codeSyncTip, t->halfLineSamplesB - t->serrPulseWidth);
      P::run(w, e->codeBlank, t->serrPulseWidth);
      break;
    }
    case CompositeLineType::BLANK_NOBURST: {
      P::run(w, e->codeSyncTip, t->hSyncWidth);
      P::run(w, e->codeBlank, t->samplesPerLine - t->hSyncWidth);
      break;
    }
    case CompositeLineType::BLANK_BURST:
    case CompositeLineType::ACTIVE: {
      // Both share the same prefix; ACTIVE splices video into the middle.
      P::run(w, e->codeSyncTip, t->hSyncWidth);
      if (t->colorEnabled && t->hBurstCycles != 0u) {
        cwBlankTo<P>(w, e, lineStart, t->hBurstStart);
        cwEmitBurst<P>(w, e, t->hBurstCycles * COMPOSITE_NUM_PHASES);
      }
      if (type == CompositeLineType::ACTIVE) {
        cwBlankTo<P>(w, e, lineStart, t->hActiveStart);
        cwEmitActive<P>(w, e, px, t->hActivePixels);
      }
      cwBlankTo<P>(w, e, lineStart, t->samplesPerLine);
      break;
    }
  }
}

}  // namespace

void compositeEmitLinePacked(CompositeSampleWriter *w,
                             const CompositeEncoder *e, CompositeLineType type,
                             const uint16_t *px) {
  emitLineT<PackedPolicy>(w, e, type, px);
}

void compositeEmitLinePwm(CompositeSampleWriter *w, const CompositeEncoder *e,
                          CompositeLineType type, const uint16_t *px) {
  emitLineT<DirectPolicy>(w, e, type, px);
}

}  // namespace lcdtap::pico2
