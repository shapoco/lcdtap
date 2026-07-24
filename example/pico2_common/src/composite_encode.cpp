#include "lcdtap/pico2/composite_encode.hpp"

// The per-line generator is force-inlined into the two public emit entry
// points, which are placed in SRAM via LCDTAP_CVBS_HOT. Inlining is what makes
// that placement effective: it pulls the policy put()/run() and the burst /
// active helpers into the sectioned functions instead of leaving them in
// flash.
#define LCDTAP_CVBS_INLINE inline __attribute__((always_inline))

namespace lcdtap::pico2 {

namespace {

// Shift-and-saturate helpers for the DIRECT_OPT sample path. On the target,
// USAT folds the >>16, the clamp at 0 and the clamp at 2^bits-1 into a single
// instruction (the operand shift is free), replacing the five-instruction
// asr/bic/cmp/it/mov sequence the generic clamp compiles to. Semantics are
// identical to the C fallback below, so host golden compares stay bit-exact.
#if defined(LCDTAP_TARGET_PICO)
LCDTAP_CVBS_INLINE uint32_t satCode7Asr16(int32_t v) {
  uint32_t r;
  asm("usat %0, #7, %1, asr #16" : "=r"(r) : "r"(v));
  return r;
}
LCDTAP_CVBS_INLINE uint32_t satCode5Asr16(int32_t v) {
  uint32_t r;
  asm("usat %0, #5, %1, asr #16" : "=r"(r) : "r"(v));
  return r;
}
// Branch-free min for two small non-negative codes (both fit a halfword).
// SSUB16 sets the GE flags from c - m, SEL then picks m when c >= m: two
// single-cycle instructions with no IT block, against the three-instruction
// cmp/it/mov the plain expression compiles to.
LCDTAP_CVBS_INLINE uint32_t minCode(uint32_t c, uint32_t m) {
  uint32_t d, scratch;
  asm("ssub16 %1, %2, %3\n\tsel %0, %3, %2"
      : "=r"(d), "=&r"(scratch)
      : "r"(c), "r"(m)
      : "cc");
  return d;
}
#else
LCDTAP_CVBS_INLINE uint32_t satCode7Asr16(int32_t v) {
  int32_t c = v >> 16;
  if (c < 0) c = 0;
  if (c > 127) c = 127;
  return static_cast<uint32_t>(c);
}
LCDTAP_CVBS_INLINE uint32_t satCode5Asr16(int32_t v) {
  int32_t c = v >> 16;
  if (c < 0) c = 0;
  if (c > 31) c = 31;
  return static_cast<uint32_t>(c);
}
LCDTAP_CVBS_INLINE uint32_t minCode(uint32_t c, uint32_t m) {
  return c < m ? c : m;
}
#endif

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

// Inline body of compositeLevelToCode(); shared so the DIRECT_NAIVE hot path
// never calls out of line (a call target in flash would defeat the SRAM
// placement of the emit functions).
LCDTAP_CVBS_INLINE uint32_t levelToCodeInner(const CompositeLevelMap &map,
                                             int32_t level) {
  level = clampLevel(level);
  uint32_t code =
      (static_cast<uint32_t>(level) * map.codeWhite + map.levelWhite / 2u) /
      map.levelWhite;
  if (code > map.codeMax) code = map.codeMax;
  return code;
}

// The chroma+luma math, per sample. Single source of truth: the LUT builder,
// the DIRECT_NAIVE path and compositeChromaSampleAt() all evaluate exactly
// this, which is what makes LUT-vs-direct equivalence testable bit for bit.
//
// Four consecutive samples carry the phases {+U, -V, -U, +V}.
//
// NTSC modulates as C = U*sin(wt) + V*cos(wt). With the burst on -U (phases
// 0 and 2 here), that places +V a quarter cycle EARLIER than +U, not later
// -- hence -v on phase 1 and +v on phase 3. Getting this backwards mirrors
// every hue about the U axis while leaving luma untouched, which is exactly
// what the first hardware test showed: skin and brown came out green, green
// came out orange, cyan came out purple. testChromaPhase() demodulates the
// colour bars against the burst and pins this down.
//
// A 90 degree absolute offset remains against the textbook form, but the
// burst carries the same offset, so the receiver -- which has no reference
// other than the burst -- cannot see it.
//
// vSign negates the V component for the PAL inverted-V lines.
LCDTAP_CVBS_INLINE uint32_t chromaSampleInner(const CompositeTiming *t,
                                              const CompositeLevelMap &map,
                                              int32_t r8, int32_t g8,
                                              int32_t b8, uint32_t phase,
                                              int32_t vSign) {
  const int32_t lumaSpan = static_cast<int32_t>(t->lvlWhite) - t->lvlBlack;
  const int32_t chromaGain = t->colorEnabled ? t->chromaGain : 0;

  const int32_t y = (77 * r8 + 150 * g8 + 29 * b8) >> 8;
  const int32_t lvl = t->lvlBlack + (y * lumaSpan) / 255;
  const int32_t u = ((b8 - y) * 126) >> 8;  // 0.492 * (B - Y)
  const int32_t v = ((r8 - y) * 225) >> 8;  // 0.879 * (R - Y)

  int32_t c;
  switch (phase & 3u) {
    case 0: c = u; break;
    case 1: c = -vSign * v; break;
    case 2: c = -u; break;
    default: c = vSign * v; break;
  }
  const int32_t off = (c * lumaSpan * chromaGain) / (255 * 256);
  return levelToCodeInner(map, clampLevel(lvl + off));
}

// --- DIRECT-mode pixel-pair sources ----------------------------------------
//
// The DIRECT emit loops are structured exactly like the LUT loops -- one
// iteration per pixel pair, four samples per iteration -- but instead of two
// LUT loads they call samples(), which fills the pair's four physical codes.
// Because every iteration advances exactly one subcarrier period, the phase
// pattern is identical for every pair in the window and all phase/sign
// decisions are hoisted to construction time.

// DIRECT_NAIVE: the reference. Re-evaluates the full division-based math per
// sample via chromaSampleInner(), so its output is bit-identical to what the
// LUT builder would have produced for the same RGB888 input.
struct NaiveChromaSrc {
  const CompositeTiming *t;
  const CompositeLevelMap *map;
  const uint16_t *px;
  uint32_t p0;    // subcarrier phase of a pair's first sample
  int32_t vSign;  // +1, or -1 on PAL inverted-V lines

  LCDTAP_CVBS_INLINE void samples(uint32_t i, uint32_t s[4]) const {
    int32_t r8, g8, b8;
    compositeRgb565To888(px[i], &r8, &g8, &b8);
    s[0] = chromaSampleInner(t, *map, r8, g8, b8, p0, vSign);
    s[1] = chromaSampleInner(t, *map, r8, g8, b8, p0 + 1u, vSign);
    compositeRgb565To888(px[i + 1u], &r8, &g8, &b8);
    s[2] = chromaSampleInner(t, *map, r8, g8, b8, p0 + 2u, vSign);
    s[3] = chromaSampleInner(t, *map, r8, g8, b8, p0 + 3u, vSign);
  }
};

// DIRECT_OPT: division-free. On the phase grid {+U, -V', -U, +V'}
// (V' = vSign*V), a pixel's two samples are always one U-axis and one V-axis
// sample; which axis comes first and with which signs depends only on the
// pair's start phase, so both are folded into (uFirst, k0, k1) at line
// entry. Pixel B of a pair sits 180 degrees later and just negates both
// coefficients. Per sample this leaves: one multiply-accumulate and one
// saturate.
//
// STATIC_CODE_MAX selects the clamp: COMPOSITE_LEVEL_MAX (127) means the
// sink's ceiling is exactly USAT #7's, so the shift and both clamps collapse
// into a single instruction (what the R-2R PAL line budget needs); -1 means
// the ceiling is the runtime codeMax (PWM, where it is clkPerSample),
// pre-clamped with USAT #5 and finished with a min. Each cursor advertises
// its value as C::DIRECT_CODE_MAX.
template <int32_t STATIC_CODE_MAX>
struct OptChromaSrc {
  const uint16_t *px;
  int32_t base;  // dirBaseQ16 (rounding pre-added)
  int32_t ky;    // dirKyQ16
  int32_t k0;    // signed Q16 coefficient, pixel A sample 0
  int32_t k1;    // signed Q16 coefficient, pixel A sample 1
  int32_t codeMax;
  bool uFirst;  // sample 0 of a pixel modulates U (else V)

  LCDTAP_CVBS_INLINE uint32_t sampleOf(int32_t lum, int32_t d,
                                       int32_t k) const {
    const int32_t t = lum + d * k;
    if (STATIC_CODE_MAX == static_cast<int32_t>(COMPOSITE_LEVEL_MAX)) {
      return satCode7Asr16(t);
    }
    if (STATIC_CODE_MAX == 31) {
      // Installed only after compositeEncoderInit() proved, over all 65536
      // colours, that no sample exceeds the runtime codeMax -- the USAT #5
      // ceiling is then unreachable and exists as a store-safety net.
      return satCode5Asr16(t);
    }
    // Runtime ceiling. Clamping to 31 first and codeMax second equals
    // clamping to codeMax directly while codeMax <= 31; encoder init rejects
    // PWM timings that would break this.
    return minCode(satCode5Asr16(t), static_cast<uint32_t>(codeMax));
  }

  // NEG negates both coefficients (pixel B of a pair, 180 degrees later).
  // It is applied to d instead of k0/k1: the negation folds into the existing
  // b8-y / r8-y subtractions for free, where negated coefficient copies cost
  // two extra live registers -- which is what previously spilled k0/k1 to the
  // stack inside the pair loop.
  template <bool NEG>
  LCDTAP_CVBS_INLINE void pixel(uint32_t v, uint32_t out[2]) const {
    int32_t r8, g8, b8;
    compositeRgb565To888(v, &r8, &g8, &b8);
    const int32_t y = (77 * r8 + 150 * g8 + 29 * b8) >> 8;
    const int32_t lum = base + y * ky;
    const int32_t db = NEG ? y - b8 : b8 - y;
    const int32_t dr = NEG ? y - r8 : r8 - y;
    out[0] = sampleOf(lum, uFirst ? db : dr, k0);
    out[1] = sampleOf(lum, uFirst ? dr : db, k1);
  }

  LCDTAP_CVBS_INLINE void samples(uint32_t i, uint32_t s[4]) const {
    pixel<false>(px[i], s);
    pixel<true>(px[i + 1u], s + 2);
  }
};

// Fold the pair's start phase and the PAL V switch into an OptChromaSrc.
// Sample slot k has type (p0 + k) & 3 on the grid {+U, -V', -U, +V'}; kvs
// carries vSign so the switch below only reasons about the grid.
template <int32_t STATIC_CODE_MAX>
LCDTAP_CVBS_INLINE OptChromaSrc<STATIC_CODE_MAX> makeOptSrc(
    const CompositeEncoder *e, const uint16_t *px, uint32_t p0,
    uint32_t vSwitch) {
  OptChromaSrc<STATIC_CODE_MAX> s;
  s.px = px;
  s.base = e->dirBaseQ16;
  s.ky = e->dirKyQ16;
  s.codeMax = static_cast<int32_t>(e->codeMax);
  const int32_t ku = e->dirKuQ16;
  const int32_t kvs = vSwitch ? -e->dirKvQ16 : e->dirKvQ16;
  switch (p0) {
    case 0u:  // slots {+U, -V'}
      s.uFirst = true;
      s.k0 = ku;
      s.k1 = -kvs;
      break;
    case 1u:  // slots {-V', -U}
      s.uFirst = false;
      s.k0 = -kvs;
      s.k1 = -ku;
      break;
    case 2u:  // slots {-U, +V'}
      s.uFirst = true;
      s.k0 = -ku;
      s.k1 = kvs;
      break;
    default:  // slots {+V', +U}
      s.uFirst = false;
      s.k0 = kvs;
      s.k1 = ku;
      break;
  }
  return s;
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
  // The 7-bit ladder saturates exactly at USAT #7's ceiling; DIRECT_OPT
  // clamps with a single instruction (see OptChromaSrc).
  static constexpr int32_t DIRECT_CODE_MAX =
      static_cast<int32_t>(COMPOSITE_LEVEL_MAX);
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

  // Active window, LUT mode. Each LUT entry already holds a pixel's two
  // samples as a packed 14-bit pair (low half: pixel starting at this phase;
  // bits 16..29: the same pixel 180 degrees later), so splice 14 bits at a
  // time instead of unpacking to four 7-bit puts. Two pixels are 28 bits --
  // exactly one word -- so nbits returns to its entry value after every pixel
  // pair, and the packing alignment can be hoisted out of the loop: four
  // branch-free inner loops, one per entry alignment. nPx is even by the
  // timing tables.
  LCDTAP_CVBS_INLINE void emitActiveLut(const CompositeEncoder *e,
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

  // Active window, DIRECT modes. Same four alignment-specialized loops as
  // emitActiveLut -- only the origin of the 14-bit pairs differs: `src`
  // computes the pair's four physical codes instead of loading them. The
  // group-alignment reasoning is unchanged.
  template <class S>
  LCDTAP_CVBS_INLINE void emitActiveDirect(const S &src, uint32_t nPx) {
    switch (nbits) {
      case 0u:
        for (uint32_t i = 0u; i < nPx; i += 2u) {
          uint32_t s[4];
          src.samples(i, s);
          const uint32_t pA = s[0] | (s[1] << 7);
          const uint32_t pB = s[2] | (s[3] << 7);
          *dst++ = pA | (pB << 14);
        }
        break;
      case 7u:
        for (uint32_t i = 0u; i < nPx; i += 2u) {
          uint32_t s[4];
          src.samples(i, s);
          const uint32_t pA = s[0] | (s[1] << 7);
          const uint32_t pB = s[2] | (s[3] << 7);
          *dst++ = acc | (pA << 7) | ((pB & 0x7Fu) << 21);
          acc = pB >> 7;
        }
        break;
      case 14u:
        for (uint32_t i = 0u; i < nPx; i += 2u) {
          uint32_t s[4];
          src.samples(i, s);
          const uint32_t pA = s[0] | (s[1] << 7);
          const uint32_t pB = s[2] | (s[3] << 7);
          *dst++ = acc | (pA << 14);
          acc = pB;
        }
        break;
      default:  // 21
        for (uint32_t i = 0u; i < nPx; i += 2u) {
          uint32_t s[4];
          src.samples(i, s);
          const uint32_t pA = s[0] | (s[1] << 7);
          const uint32_t pB = s[2] | (s[3] << 7);
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
  // The ceiling is the runtime clkPerSample; DIRECT_OPT uses the two-step
  // USAT #5 + min clamp (see OptChromaSrc).
  static constexpr int32_t DIRECT_CODE_MAX = -1;

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

  // Active window, LUT mode. The PWM LUT stores ready-made 32-bit words (two
  // 16-bit samples each): one load and one store per pixel, no unpacking. The
  // word alignment of dst is constant across the window, so it is resolved
  // once.
  LCDTAP_CVBS_INLINE void emitActiveLut(const CompositeEncoder *e,
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

  // Active window, DIRECT modes. Identical store structure to emitActiveLut;
  // the four codes per pair come from `src` instead of the LUT.
  template <class S>
  LCDTAP_CVBS_INLINE void emitActiveDirect(const S &src, uint32_t nPx) {
    if ((reinterpret_cast<uintptr_t>(dst) & 3u) == 0u) {
      uint32_t *p = reinterpret_cast<uint32_t *>(dst);
      for (uint32_t i = 0u; i < nPx; i += 2u) {
        uint32_t s[4];
        src.samples(i, s);
        *p++ = s[0] | (s[1] << 16);
        *p++ = s[2] | (s[3] << 16);
      }
      dst = reinterpret_cast<uint16_t *>(p);
    } else {
      for (uint32_t i = 0u; i < nPx; i += 2u) {
        uint32_t s[4];
        src.samples(i, s);
        dst[0] = static_cast<uint16_t>(s[0]);
        dst[1] = static_cast<uint16_t>(s[1]);
        dst[2] = static_cast<uint16_t>(s[2]);
        dst[3] = static_cast<uint16_t>(s[3]);
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
  return levelToCodeInner(map, level);
}

uint32_t compositeChromaSampleAt(const CompositeTiming *timing,
                                 const CompositeLevelMap &map, int32_t r8,
                                 int32_t g8, int32_t b8, uint32_t phase,
                                 int32_t vSign) {
  return chromaSampleInner(timing, map, r8, g8, b8, phase, vSign);
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
                           const CompositeDacProfile *dac,
                           CompositeChromaMode chromaMode) {
  // The DIRECT modes compute per pixel and use no LUT at all.
  if (chromaMode != CompositeChromaMode::LUT) return 0u;
  // The PWM layout stores two ready-made words per colour (see the lut
  // comment in composite_encode.hpp); the R-2R layout packs one.
  const uint32_t wordsPerColor = dac->kind == CompositeDacKind::PWM ? 2u : 1u;
  return lutSetsOf(timing) * COMPOSITE_LUT_COLORS * wordsPerColor;
}

bool compositeEncoderInit(CompositeEncoder *e, const CompositeTiming *timing,
                          const CompositeDacProfile *dac, uint32_t *lutStorage,
                          CompositeChromaMode chromaMode) {
  if (timing == nullptr || dac == nullptr) return false;
  if (chromaMode == CompositeChromaMode::LUT && lutStorage == nullptr)
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

  // One emit entry point per sink x chroma mode, so each SRAM-resident
  // function carries only its own active-pixel path.
  static const CompositeEmitLineFn PACKED_FNS[3] = {
      compositeEmitLinePacked, compositeEmitLinePackedNaive,
      compositeEmitLinePackedOpt};
  static const CompositeEmitLineFn PWM_FNS[3] = {
      compositeEmitLinePwm, compositeEmitLinePwmNaive, compositeEmitLinePwmOpt};

  e->chromaMode = chromaMode;
  if (dac->kind == CompositeDacKind::PWM) {
    e->bitsPerSample = 0u;
    e->flushBits = 0u;
    e->emitLine = PWM_FNS[static_cast<int>(chromaMode)];
  } else {
    // R2rCursor hardcodes the 7-bit packing geometry; the only ladder is the
    // 7-bit one and there are no plans for another, so reject anything else
    // rather than carrying runtime-width packing on the hot path.
    if (dac->bits != COMPOSITE_LEVEL_BITS) return false;
    e->bitsPerSample = dac->bits;
    e->flushBits = e->samplesPerTransfer * dac->bits;
    e->emitLine = PACKED_FNS[static_cast<int>(chromaMode)];
  }

  const CompositeLevelMap map = compositeLevelMap(timing, dac);
  e->codeMax = map.codeMax;
  // The LUT packs samples into 7-bit fields, so no physical code may exceed
  // that. Holds for both sinks (127 and 22/18), but assert rather than assume.
  if (map.codeMax > COMPOSITE_LEVEL_MAX) return false;
  // The PWM DIRECT_OPT clamp pre-saturates with USAT #5, which is only
  // equivalent to a direct codeMax clamp while codeMax <= 31. Holds for every
  // timing (codeMax = clkPerSample), but assert rather than assume.
  if (chromaMode == CompositeChromaMode::DIRECT_OPT &&
      dac->kind == CompositeDacKind::PWM && map.codeMax > 31u) {
    return false;
  }

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

  e->lutSets = lutSetsOf(timing);
  e->map = map;

  // DIRECT_OPT constants. All divisions of the per-sample math are folded
  // here, once, in 64-bit and rounded to nearest:
  //
  //   naive: lvl = lvlBlack + y*lumaSpan/255
  //          off = c * lumaSpan * chromaGain / (255*256),  c = d*126>>8 etc.
  //          code = (lvl+off) * codeWhite / levelWhite     (level->code map)
  //
  // fold the level->code scale into every term and express the chroma path
  // directly in terms of d = (B-Y) or (R-Y):
  //
  //   codeQ16 = dirBaseQ16 + y*dirKyQ16 +- d*dirK{u,v}Q16
  //
  // dirBaseQ16 pre-adds 0x8000 so the final >>16 rounds instead of
  // truncating. The naive path truncates several intermediates instead, which
  // is where the +-2 code tolerance between the two comes from.
  {
    const int64_t Q = 65536;
    const int32_t lumaSpan =
        static_cast<int32_t>(timing->lvlWhite) - timing->lvlBlack;
    const int32_t chromaGain = timing->colorEnabled ? timing->chromaGain : 0;
    const int64_t cw = map.codeWhite;
    const int64_t lw = map.levelWhite;
    const int64_t denomC = 256LL * 255 * 256 * lw;
    e->dirBaseQ16 = static_cast<int32_t>(
        (static_cast<int64_t>(timing->lvlBlack) * cw * Q + lw / 2) / lw);
    e->dirBaseQ16 += 32768;
    e->dirKyQ16 = static_cast<int32_t>(
        (static_cast<int64_t>(lumaSpan) * cw * Q + (255 * lw) / 2) /
        (255 * lw));
    e->dirKuQ16 = static_cast<int32_t>(
        (126LL * lumaSpan * chromaGain * cw * Q + denomC / 2) / denomC);
    e->dirKvQ16 = static_cast<int32_t>(
        (225LL * lumaSpan * chromaGain * cw * Q + denomC / 2) / denomC);
  }

  // PWM DIRECT_OPT: the runtime codeMax min costs two instructions per sample
  // in the hottest loop -- decisive on the PAL line budget. The level design
  // (pwmCcWhite) keeps peak chroma under codeMax, so the clamp should never
  // fire; prove that exhaustively over every RGB565 colour and both chroma
  // axes with the exact hot-path integer math, and only then install the
  // clamp-free variant. Anything out of range falls back to the clamping one,
  // so the two builds are output-identical by construction.
  if (chromaMode == CompositeChromaMode::DIRECT_OPT &&
      dac->kind == CompositeDacKind::PWM) {
    bool fits = true;
    for (uint32_t v = 0u; v < 65536u && fits; ++v) {
      int32_t r8, g8, b8;
      compositeRgb565To888(v, &r8, &g8, &b8);
      const int32_t y = (77 * r8 + 150 * g8 + 29 * b8) >> 8;
      const int32_t lum = e->dirBaseQ16 + y * e->dirKyQ16;
      const int32_t ds[2] = {b8 - y, r8 - y};
      const int32_t ks[2] = {e->dirKuQ16, e->dirKvQ16};
      for (int i = 0; i < 2 && fits; ++i) {
        const int32_t hi = (lum + ds[i] * ks[i]) >> 16;
        const int32_t lo = (lum - ds[i] * ks[i]) >> 16;
        if (hi > static_cast<int32_t>(map.codeMax) ||
            lo > static_cast<int32_t>(map.codeMax)) {
          fits = false;
        }
      }
    }
    if (fits) e->emitLine = compositeEmitLinePwmOptNoClamp;
  }

  // The DIRECT modes compute per pixel; no table to build.
  if (chromaMode != CompositeChromaMode::LUT) {
    e->lut = nullptr;
    return true;
  }

  e->lut = lutStorage;
  const bool pwmLut = dac->kind == CompositeDacKind::PWM;

  // Sets 0..3 are the four subcarrier phases. For PAL colour, sets 4..7 repeat
  // them with the V axis inverted -- the line-alternating switch that
  // emitActiveLut() selects between via vSwitch. NTSC has only the lower four.
  // The per-sample math lives in chromaSampleInner() (see the phase-sign
  // discussion there), shared verbatim with the DIRECT_NAIVE path.
  for (uint32_t set = 0u; set < e->lutSets; ++set) {
    uint32_t *dst = e->lut + set * COMPOSITE_LUT_COLORS * (pwmLut ? 2u : 1u);
    const uint32_t phase0 = set & 3u;
    const int32_t vSign = (set >= COMPOSITE_NUM_PHASES) ? -1 : 1;
    for (uint32_t idx = 0u; idx < COMPOSITE_LUT_COLORS; ++idx) {
      const int32_t r8 = static_cast<int32_t>(((idx >> 5) & 7u) * 255u / 7u);
      const int32_t g8 = static_cast<int32_t>(((idx >> 2) & 7u) * 255u / 7u);
      const int32_t b8 = static_cast<int32_t>((idx & 3u) * 255u / 3u);

      uint32_t s[COMPOSITE_NUM_PHASES];
      for (uint32_t k = 0u; k < COMPOSITE_NUM_PHASES; ++k) {
        s[k] = chromaSampleInner(timing, map, r8, g8, b8, phase0 + k, vSign);
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

// The line structure, written once and instantiated per sink cursor and
// chroma mode. All writer state stays in the cursor (registers) from here to
// the end of the line; see the cursor block above for why. MODE is a
// compile-time constant (0 = LUT, 1 = DIRECT_NAIVE, 2 = DIRECT_OPT), so the
// mode branches below fold away and each public entry point contains only
// its own active-pixel path.
// OPT_MAX overrides the cursor's default clamp for the DIRECT_OPT path; the
// PWM no-clamp entry point passes 31 (see compositeEmitLinePwmOptNoClamp).
template <class C, int MODE, int32_t OPT_MAX = C::DIRECT_CODE_MAX>
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
        if (MODE == 0) {
          c.emitActiveLut(e, vSwitch, px, t->hActivePixels);
        } else if (MODE == 1) {
          const NaiveChromaSrc src = {t, &e->map, px, c.phase & 3u,
                                      vSwitch ? -1 : 1};
          c.emitActiveDirect(src, t->hActivePixels);
        } else {
          const OptChromaSrc<OPT_MAX> src =
              makeOptSrc<OPT_MAX>(e, px, c.phase & 3u, vSwitch);
          c.emitActiveDirect(src, t->hActivePixels);
        }
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
  emitLineT<R2rCursor, 0>(w, e, type, line, px);
}

void LCDTAP_CVBS_HOT(compositeEmitLinePackedNaive)(CompositeSampleWriter *w,
                                                   const CompositeEncoder *e,
                                                   CompositeLineType type,
                                                   uint32_t line,
                                                   const uint16_t *px) {
  emitLineT<R2rCursor, 1>(w, e, type, line, px);
}

void LCDTAP_CVBS_HOT(compositeEmitLinePackedOpt)(CompositeSampleWriter *w,
                                                 const CompositeEncoder *e,
                                                 CompositeLineType type,
                                                 uint32_t line,
                                                 const uint16_t *px) {
  emitLineT<R2rCursor, 2>(w, e, type, line, px);
}

void LCDTAP_CVBS_HOT(compositeEmitLinePwm)(CompositeSampleWriter *w,
                                           const CompositeEncoder *e,
                                           CompositeLineType type,
                                           uint32_t line, const uint16_t *px) {
  emitLineT<PwmCursor, 0>(w, e, type, line, px);
}

void LCDTAP_CVBS_HOT(compositeEmitLinePwmNaive)(CompositeSampleWriter *w,
                                                const CompositeEncoder *e,
                                                CompositeLineType type,
                                                uint32_t line,
                                                const uint16_t *px) {
  emitLineT<PwmCursor, 1>(w, e, type, line, px);
}

void LCDTAP_CVBS_HOT(compositeEmitLinePwmOpt)(CompositeSampleWriter *w,
                                              const CompositeEncoder *e,
                                              CompositeLineType type,
                                              uint32_t line,
                                              const uint16_t *px) {
  emitLineT<PwmCursor, 2>(w, e, type, line, px);
}

void LCDTAP_CVBS_HOT(compositeEmitLinePwmOptNoClamp)(CompositeSampleWriter *w,
                                                     const CompositeEncoder *e,
                                                     CompositeLineType type,
                                                     uint32_t line,
                                                     const uint16_t *px) {
  emitLineT<PwmCursor, 2, 31>(w, e, type, line, px);
}

}  // namespace lcdtap::pico2
