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

// Map a normalized 7-bit level onto the physical DAC code space.
uint32_t levelToCode(const CompositeDacProfile *dac, int32_t level) {
  level = clampLevel(level);
  if (dac->bits >= COMPOSITE_LEVEL_BITS) return static_cast<uint32_t>(level);

  // Narrow weighted DAC: pick the code whose level is nearest.
  const uint32_t n = 1u << dac->bits;
  uint32_t best = 0u;
  int32_t bestErr = 0x7FFFFFFF;
  for (uint32_t i = 0u; i < n; ++i) {
    int32_t err = static_cast<int32_t>(dac->levels[i]) - level;
    if (err < 0) err = -err;
    if (err < bestErr) {
      bestErr = err;
      best = i;
    }
  }
  return best;
}

// Append one sample. nbits never spans a word because flushBits is always an
// exact multiple of bitsPerSample.
inline void cwPut(CompositeSampleWriter *w, uint32_t code) {
  w->acc |= code << w->nbits;
  w->nbits += w->bitsPerSample;
  if (w->nbits >= w->flushBits) {
    *w->dst++ = w->acc;
    w->acc = 0u;
    w->nbits = 0u;
  }
  w->sampleIndex++;
}

// Emit `n` samples at a constant level. Whole words are stored directly.
void cwEmitRun(CompositeSampleWriter *w, uint32_t code, uint32_t n) {
  while (n != 0u && w->nbits != 0u) {
    cwPut(w, code);
    --n;
  }
  if (n >= w->samplesPerWord) {
    uint32_t packed = 0u;
    for (uint32_t i = 0u; i < w->samplesPerWord; ++i) {
      packed |= code << (i * w->bitsPerSample);
    }
    const uint32_t words = n / w->samplesPerWord;
    for (uint32_t i = 0u; i < words; ++i) *w->dst++ = packed;
    const uint32_t done = words * w->samplesPerWord;
    w->sampleIndex += done;
    n -= done;
  }
  while (n != 0u) {
    cwPut(w, code);
    --n;
  }
}

// Emit the colour burst: `n` samples modulated at the subcarrier frequency,
// taking the phase from the absolute sample index.
void cwEmitBurst(CompositeSampleWriter *w, const CompositeEncoder *e,
                 uint32_t n) {
  for (uint32_t i = 0u; i < n; ++i) {
    cwPut(w, e->burstCode[w->sampleIndex & 3u]);
  }
}

// Emit the active window. Two samples per pixel means consecutive pixels
// start 180 degrees apart, so they alternate between the two halves of a LUT
// entry. nPx is guaranteed even by the timing tables.
void cwEmitActive(CompositeSampleWriter *w, const CompositeEncoder *e,
                  const uint16_t *px, uint32_t nPx) {
  const uint32_t *lutSet =
      e->lut + (w->sampleIndex & 3u) * COMPOSITE_LUT_COLORS;
  for (uint32_t i = 0u; i < nPx; i += 2u) {
    const uint32_t a = lutSet[rgb565ToIndex(px[i])];
    const uint32_t b = lutSet[rgb565ToIndex(px[i + 1u])];
    cwPut(w, a & 0x7Fu);
    cwPut(w, (a >> 7) & 0x7Fu);
    cwPut(w, (b >> 16) & 0x7Fu);
    cwPut(w, (b >> 23) & 0x7Fu);
  }
}

// Blank from the current position up to `target` samples into the line.
inline void cwBlankTo(CompositeSampleWriter *w, const CompositeEncoder *e,
                      uint32_t lineStart, uint32_t target) {
  const uint32_t at = w->sampleIndex - lineStart;
  if (target > at) cwEmitRun(w, e->codeBlank, target - at);
}

}  // namespace

uint32_t compositeLinesPerSlot(const CompositeTiming *timing,
                               const CompositeDacProfile *dac) {
  const uint32_t samplesPerWord = 32u / dac->bits;
  return samplesPerWord / gcd(samplesPerWord, timing->samplesPerLine);
}

uint32_t compositeWordsPerSlot(const CompositeTiming *timing,
                               const CompositeDacProfile *dac) {
  const uint32_t samplesPerWord = 32u / dac->bits;
  return compositeLinesPerSlot(timing, dac) * timing->samplesPerLine /
         samplesPerWord;
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
  if (dac->bits == 0u || dac->bits > COMPOSITE_LEVEL_BITS) return false;
  if ((timing->hActivePixels & 1u) != 0u) return false;
  if (timing->hActiveSamples != timing->hActivePixels * 2u) return false;

  e->timing = timing;
  e->dac = dac;
  e->bitsPerSample = dac->bits;
  e->samplesPerWord = 32u / dac->bits;
  e->flushBits = e->samplesPerWord * dac->bits;
  e->linesPerSlot = compositeLinesPerSlot(timing, dac);
  e->wordsPerSlot = compositeWordsPerSlot(timing, dac);
  if (e->linesPerSlot > COMPOSITE_MAX_LINES_PER_SLOT) return false;

  e->codeSyncTip = levelToCode(dac, timing->lvlSyncTip);
  e->codeBlank = levelToCode(dac, timing->lvlBlank);

  // Burst sits on the -U axis. On a 4x fsc grid the quadrature mixer
  // degenerates to cos = {+1, 0, -1, 0}, sin = {0, +1, 0, -1}, so a 180
  // degree burst is simply the blanking level +/- the burst amplitude.
  const int32_t blank = timing->lvlBlank;
  const int32_t amp = timing->burstAmplitude;
  const bool burst = timing->colorEnabled && timing->hBurstCycles != 0u;
  e->burstCode[0] = levelToCode(dac, burst ? blank - amp : blank);
  e->burstCode[1] = levelToCode(dac, blank);
  e->burstCode[2] = levelToCode(dac, burst ? blank + amp : blank);
  e->burstCode[3] = levelToCode(dac, blank);

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
        s[k] = levelToCode(dac, clampLevel(lvl + off));
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
                         uint32_t *dst) {
  w->dst = dst;
  w->acc = 0u;
  w->nbits = 0u;
  w->sampleIndex = 0u;
  w->bitsPerSample = e->bitsPerSample;
  w->samplesPerWord = e->samplesPerWord;
  w->flushBits = e->flushBits;
}

void compositeEmitLine(CompositeSampleWriter *w, const CompositeEncoder *e,
                       CompositeLineType type, const uint16_t *px) {
  const CompositeTiming *t = e->timing;
  const uint32_t lineStart = w->sampleIndex;

  switch (type) {
    case CompositeLineType::EQ: {
      // Two narrow sync pulses, one per half line.
      cwEmitRun(w, e->codeSyncTip, t->eqPulseWidth);
      cwEmitRun(w, e->codeBlank, t->halfLineSamplesA - t->eqPulseWidth);
      cwEmitRun(w, e->codeSyncTip, t->eqPulseWidth);
      cwEmitRun(w, e->codeBlank, t->halfLineSamplesB - t->eqPulseWidth);
      break;
    }
    case CompositeLineType::SERR: {
      // Two broad pulses: sync tip for most of each half line.
      cwEmitRun(w, e->codeSyncTip, t->halfLineSamplesA - t->serrPulseWidth);
      cwEmitRun(w, e->codeBlank, t->serrPulseWidth);
      cwEmitRun(w, e->codeSyncTip, t->halfLineSamplesB - t->serrPulseWidth);
      cwEmitRun(w, e->codeBlank, t->serrPulseWidth);
      break;
    }
    case CompositeLineType::BLANK_NOBURST: {
      cwEmitRun(w, e->codeSyncTip, t->hSyncWidth);
      cwEmitRun(w, e->codeBlank, t->samplesPerLine - t->hSyncWidth);
      break;
    }
    case CompositeLineType::BLANK_BURST:
    case CompositeLineType::ACTIVE: {
      // Both share the same prefix; ACTIVE splices video into the middle.
      cwEmitRun(w, e->codeSyncTip, t->hSyncWidth);
      if (t->colorEnabled && t->hBurstCycles != 0u) {
        cwBlankTo(w, e, lineStart, t->hBurstStart);
        cwEmitBurst(w, e, t->hBurstCycles * COMPOSITE_NUM_PHASES);
      }
      if (type == CompositeLineType::ACTIVE) {
        cwBlankTo(w, e, lineStart, t->hActiveStart);
        cwEmitActive(w, e, px, t->hActivePixels);
      }
      cwBlankTo(w, e, lineStart, t->samplesPerLine);
      break;
    }
  }
}

}  // namespace lcdtap::pico2
