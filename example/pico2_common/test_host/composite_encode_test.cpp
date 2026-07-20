// Host-side unit test for composite_encode.cpp (no MCU dependencies).
//
// Generates whole NTSC/PAL fields, unpacks the DAC sample stream back out of
// the DMA word format, and checks the structural invariants that are painful
// to diagnose on an oscilloscope: group alignment, sample counts, sync and
// burst geometry, subcarrier phase coherence, and signal levels.
//
// Build & run:
//   g++ -O2 -Wall -Wextra -I../include -o /tmp/composite_encode_test
//       composite_encode_test.cpp ../src/composite_encode.cpp
//       ../src/composite_timing.cpp
//   /tmp/composite_encode_test

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "lcdtap/pico2/composite_encode.hpp"

using namespace lcdtap::pico2;

static int gFailures = 0;

#define CHECK(cond, ...)                            \
  do {                                              \
    if (!(cond)) {                                  \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                          \
      printf("\n");                                 \
      gFailures++;                                  \
    }                                               \
  } while (0)

namespace {

// One generated field, unpacked back into individual DAC codes.
struct Field {
  std::vector<uint8_t> samples;  // one physical DAC code per entry
  std::vector<uint32_t> words;   // the raw DMA stream
  uint32_t wordsPerSlot = 0;
  uint32_t linesPerSlot = 0;
};

// A recognisable test pattern: eight vertical colour bars.
std::vector<uint16_t> makeColorBars(uint32_t nPx) {
  static const uint16_t kBars[8] = {
      0xFFFF,  // white
      0xFFE0,  // yellow
      0x07FF,  // cyan
      0x07E0,  // green
      0xF81F,  // magenta
      0xF800,  // red
      0x001F,  // blue
      0x0000,  // black
  };
  std::vector<uint16_t> px(nPx);
  for (uint32_t i = 0; i < nPx; ++i) px[i] = kBars[(i * 8) / nPx];
  return px;
}

Field generateField(const CompositeTiming *t, const CompositeDacProfile *dac) {
  Field f;
  std::vector<uint32_t> lut(compositeLutWords(t));
  CompositeEncoder enc;
  if (!compositeEncoderInit(&enc, t, dac, lut.data())) {
    printf("  FAIL compositeEncoderInit(%s, %s) returned false\n", t->name,
           dac->name);
    gFailures++;
    return f;
  }
  f.wordsPerSlot = enc.wordsPerSlot;
  f.linesPerSlot = enc.linesPerSlot;

  const std::vector<uint16_t> px = makeColorBars(t->hActivePixels);
  const uint32_t numSlots = t->vTotalLines / enc.linesPerSlot;

  CHECK(t->vTotalLines % enc.linesPerSlot == 0,
        "%s/%s: vTotalLines %u is not a multiple of linesPerSlot %u", t->name,
        dac->name, t->vTotalLines, enc.linesPerSlot);

  std::vector<uint32_t> slot(enc.wordsPerSlot);
  for (uint32_t s = 0; s < numSlots; ++s) {
    CompositeSampleWriter w;
    compositeWriterInit(&w, &enc, slot.data());
    for (uint32_t k = 0; k < enc.linesPerSlot; ++k) {
      uint32_t y = 0;
      const uint32_t line = s * enc.linesPerSlot + k;
      const CompositeLineType type = compositeClassifyLine(t, line, &y);
      compositeEmitLine(&w, &enc, type, px.data());
    }
    // The whole point of the group structure: a group always ends exactly on
    // a word boundary with nothing left in the accumulator.
    CHECK(w.nbits == 0, "%s/%s slot %u: %u bits left over", t->name, dac->name,
          s, w.nbits);
    CHECK(w.dst == slot.data() + enc.wordsPerSlot,
          "%s/%s slot %u: wrote %ld words, expected %u", t->name, dac->name, s,
          (long)(w.dst - slot.data()), enc.wordsPerSlot);
    CHECK(w.sampleIndex == enc.linesPerSlot * t->samplesPerLine,
          "%s/%s slot %u: emitted %u samples, expected %u", t->name, dac->name,
          s, w.sampleIndex, enc.linesPerSlot * t->samplesPerLine);

    f.words.insert(f.words.end(), slot.begin(), slot.end());
    // Unpack back to individual samples.
    const uint32_t mask = (1u << dac->bits) - 1u;
    for (uint32_t i = 0; i < enc.wordsPerSlot; ++i) {
      for (uint32_t j = 0; j < enc.samplesPerWord; ++j) {
        f.samples.push_back((slot[i] >> (j * dac->bits)) & mask);
      }
    }
  }
  return f;
}

double usPerSample(const CompositeTiming *t) {
  const double sampleHz = (double)t->clkSysKhz * 1000.0 / t->pioClkdivInt;
  return 1e6 / sampleHz;
}

// --- structural checks -----------------------------------------------------

void testGeometry() {
  printf("geometry\n");
  struct Case {
    const CompositeTiming *t;
    const CompositeDacProfile *d;
    uint32_t linesPerSlot;
    uint32_t wordsPerSlot;
  };
  const Case cases[] = {
      {&COMPOSITE_TIMING_NTSC_J_240P, &COMPOSITE_DAC_7BIT_GPIO5, 2, 455},
      {&COMPOSITE_TIMING_PAL_B_288P, &COMPOSITE_DAC_7BIT_GPIO5, 4, 1135},
      {&COMPOSITE_TIMING_NTSC_J_240P, &COMPOSITE_DAC_3BIT_GPIO5, 1, 91},
      {&COMPOSITE_TIMING_PAL_B_288P, &COMPOSITE_DAC_3BIT_GPIO5, 2, 227},
  };
  for (const Case &c : cases) {
    const uint32_t lps = compositeLinesPerSlot(c.t, c.d);
    const uint32_t wps = compositeWordsPerSlot(c.t, c.d);
    CHECK(lps == c.linesPerSlot, "%s/%s linesPerSlot %u, expected %u",
          c.t->name, c.d->name, lps, c.linesPerSlot);
    CHECK(wps == c.wordsPerSlot, "%s/%s wordsPerSlot %u, expected %u",
          c.t->name, c.d->name, wps, c.wordsPerSlot);
    // The group must hold a whole number of both words and lines.
    const uint32_t samplesPerWord = 32u / c.d->bits;
    CHECK(wps * samplesPerWord == lps * c.t->samplesPerLine,
          "%s/%s: %u words * %u != %u lines * %u samples", c.t->name, c.d->name,
          wps, samplesPerWord, lps, c.t->samplesPerLine);
  }
}

void testTimingTables() {
  printf("timing tables\n");
  const CompositeTiming *ts[] = {&COMPOSITE_TIMING_NTSC_J_240P,
                                 &COMPOSITE_TIMING_PAL_B_288P};
  for (const CompositeTiming *t : ts) {
    // The PLL recipe must actually produce the stated clk_sys.
    const uint32_t vco = 12000u * t->pllFbdiv / t->pllRefdiv;  // kHz
    const uint32_t sys = vco / (t->pllPostdiv1 * t->pllPostdiv2);
    CHECK(sys == t->clkSysKhz, "%s: PLL yields %u kHz, table says %u", t->name,
          sys, t->clkSysKhz);
    CHECK(vco >= 750000u && vco <= 1600000u, "%s: VCO %u kHz out of range",
          t->name, vco);

    // Sample rate must be 4x fsc.
    const double fsc =
        (t == &COMPOSITE_TIMING_NTSC_J_240P) ? 315e6 / 88.0 : 4433618.75;
    const double sampleHz = (double)t->clkSysKhz * 1000.0 / t->pioClkdivInt;
    const double ppm = (sampleHz - 4.0 * fsc) / (4.0 * fsc) * 1e6;
    CHECK(std::fabs(ppm) < 100.0, "%s: sample rate is %.1f ppm off 4x fsc",
          t->name, ppm);
    printf("  %-12s %.6f MHz (%+.1f ppm), line %.4f us\n", t->name,
           sampleHz / 1e6, ppm, t->samplesPerLine * usPerSample(t));

    // The encoder writes two pixels per iteration and two samples per pixel.
    CHECK((t->hActivePixels & 1u) == 0, "%s: hActivePixels %u is odd", t->name,
          t->hActivePixels);
    CHECK(t->hActiveSamples == t->hActivePixels * 2u,
          "%s: hActiveSamples %u != 2 * hActivePixels %u", t->name,
          t->hActiveSamples, t->hActivePixels);
    // Everything must fit inside the line.
    CHECK(t->hActiveStart + t->hActiveSamples <= t->samplesPerLine,
          "%s: active window overruns the line", t->name);
    CHECK(t->hBurstStart + t->hBurstCycles * 4u <= t->hActiveStart,
          "%s: burst overlaps active video", t->name);
    CHECK(t->hSyncWidth < t->hBurstStart, "%s: burst starts inside sync",
          t->name);
    CHECK(t->halfLineSamplesA + t->halfLineSamplesB == t->samplesPerLine,
          "%s: half lines %u + %u != %u", t->name, t->halfLineSamplesA,
          t->halfLineSamplesB, t->samplesPerLine);
    CHECK(t->vPreEqLines + t->vSerrLines + t->vPostEqLines + t->vActiveLines <=
              t->vTotalLines,
          "%s: vertical structure overruns the field", t->name);
  }
}

void testFieldStructure() {
  printf("field structure\n");
  const CompositeTiming *ts[] = {&COMPOSITE_TIMING_NTSC_J_240P,
                                 &COMPOSITE_TIMING_PAL_B_288P};
  for (const CompositeTiming *t : ts) {
    const Field f = generateField(t, &COMPOSITE_DAC_7BIT_GPIO5);
    if (f.samples.empty()) continue;

    const uint32_t expect = t->vTotalLines * t->samplesPerLine;
    CHECK(f.samples.size() == expect, "%s: field has %zu samples, expected %u",
          t->name, f.samples.size(), expect);

    // The subcarrier phase must be continuous across the field boundary,
    // which requires the field to be a whole number of subcarrier cycles.
    CHECK(expect % 4u == 0, "%s: field is %u samples, not a multiple of 4",
          t->name, expect);

    // Every line must begin with a sync tip and contain exactly one sync
    // interval of the expected width (equalizing/serration lines aside).
    const uint32_t activeStart = t->vTotalLines - t->vActiveLines;
    for (uint32_t line = activeStart; line < t->vTotalLines; ++line) {
      const uint8_t *s = &f.samples[line * t->samplesPerLine];
      for (uint32_t i = 0; i < t->hSyncWidth; ++i) {
        if (s[i] != t->lvlSyncTip) {
          CHECK(false, "%s line %u: sample %u is %u, expected sync tip %u",
                t->name, line, i, s[i], t->lvlSyncTip);
          break;
        }
      }
      CHECK(s[t->hSyncWidth] != t->lvlSyncTip,
            "%s line %u: sync tip extends past hSyncWidth %u", t->name, line,
            t->hSyncWidth);
    }
    printf("  %-12s %zu samples, %.4f us sync, %.4f us line\n", t->name,
           f.samples.size(), t->hSyncWidth * usPerSample(t),
           t->samplesPerLine * usPerSample(t));
  }
}

void testBurst() {
  printf("colour burst\n");
  // Only NTSC carries a burst in Phase 1; PAL is monochrome for now.
  const CompositeTiming *t = &COMPOSITE_TIMING_NTSC_J_240P;
  const Field f = generateField(t, &COMPOSITE_DAC_7BIT_GPIO5);
  if (f.samples.empty()) return;

  const uint32_t activeStart = t->vTotalLines - t->vActiveLines;
  for (uint32_t line = activeStart; line < t->vTotalLines; ++line) {
    const uint32_t base = line * t->samplesPerLine;
    const uint8_t *s = &f.samples[base + t->hBurstStart];
    for (uint32_t i = 0; i < t->hBurstCycles * 4u; ++i) {
      // Phase comes from the absolute sample index, so burst and chroma
      // share one reference. This is what makes PAL colour tractable later.
      const uint32_t phase = (base + t->hBurstStart + i) & 3u;
      int expect = t->lvlBlank;
      if (phase == 0) expect = t->lvlBlank - t->burstAmplitude;
      if (phase == 2) expect = t->lvlBlank + t->burstAmplitude;
      if (s[i] != expect) {
        CHECK(false, "%s line %u burst sample %u: %u, expected %d (phase %u)",
              t->name, line, i, s[i], expect, phase);
        return;
      }
    }
  }
  // The burst must be suppressed through the vertical interval.
  for (uint32_t line = 0; line < t->vBurstBlankEnd; ++line) {
    const uint8_t *s = &f.samples[line * t->samplesPerLine + t->hBurstStart];
    bool modulated = false;
    for (uint32_t i = 0; i < t->hBurstCycles * 4u; ++i) {
      if (s[i] != t->lvlBlank && s[i] != t->lvlSyncTip) modulated = true;
    }
    CHECK(!modulated, "%s line %u: burst present during vertical blanking",
          t->name, line);
  }
  printf("  NTSC-J      %u cycles at %.2f us, amplitude +-%u\n",
         t->hBurstCycles, t->hBurstStart * usPerSample(t), t->burstAmplitude);
}

void testLevels() {
  printf("signal levels\n");
  const CompositeTiming *t = &COMPOSITE_TIMING_NTSC_J_240P;
  const Field f = generateField(t, &COMPOSITE_DAC_7BIT_GPIO5);
  if (f.samples.empty()) return;

  // Nothing may exceed the DAC range, on any line.
  uint8_t lo = 255, hi = 0;
  for (uint8_t v : f.samples) {
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  CHECK(hi <= COMPOSITE_LEVEL_MAX, "peak code %u exceeds %u", hi,
        COMPOSITE_LEVEL_MAX);
  CHECK(lo == t->lvlSyncTip, "minimum code is %u, expected sync tip %u", lo,
        t->lvlSyncTip);
  printf("  code range %u..%u (sync %u, blank %u, white %u)\n", lo, hi,
         t->lvlSyncTip, t->lvlBlank, t->lvlWhite);

  // White and black bars must land on the nominal levels: the leftmost bar is
  // white, the rightmost is black, and neither carries chroma.
  const uint32_t line = t->vTotalLines - 1;
  const uint8_t *a = &f.samples[line * t->samplesPerLine + t->hActiveStart];
  const uint32_t lastBar = t->hActiveSamples - 8;
  CHECK(a[8] == t->lvlWhite, "white bar reads %u, expected %u", a[8],
        t->lvlWhite);
  CHECK(a[lastBar] == t->lvlBlack, "black bar reads %u, expected %u",
        a[lastBar], t->lvlBlack);
}

void testMonochrome() {
  printf("monochrome (PAL phase 1, and the 3-bit I2C DAC)\n");
  // A monochrome timing must produce no chroma modulation at all: every
  // active sample pair for a given pixel must be identical.
  const CompositeTiming *t = &COMPOSITE_TIMING_PAL_B_288P;
  const Field f = generateField(t, &COMPOSITE_DAC_3BIT_GPIO5);
  if (f.samples.empty()) return;

  const uint32_t line = t->vTotalLines - 1;
  const uint8_t *a = &f.samples[line * t->samplesPerLine + t->hActiveStart];
  for (uint32_t i = 0; i < t->hActiveSamples; i += 2) {
    if (a[i] != a[i + 1]) {
      CHECK(false, "%s: chroma present in monochrome mode at sample %u",
            t->name, i);
      break;
    }
  }
  // The 3-bit DAC must only ever emit codes 0..7.
  for (uint8_t v : f.samples) {
    if (v > 7) {
      CHECK(false, "3-bit DAC emitted code %u", v);
      break;
    }
  }
  printf("  PAL-B/3bit  no chroma, codes within 0..7\n");
}

void testDacProfiles() {
  printf("DAC profiles\n");
  const CompositeTiming *ts[] = {&COMPOSITE_TIMING_NTSC_J_240P,
                                 &COMPOSITE_TIMING_PAL_B_288P};
  const CompositeDacProfile *ds[] = {&COMPOSITE_DAC_7BIT_GPIO5,
                                     &COMPOSITE_DAC_3BIT_GPIO5};
  for (const CompositeDacProfile *d : ds) {
    const uint32_t n = 1u << d->bits;
    if (d->bits < COMPOSITE_LEVEL_BITS) {
      // Levels must be ascending so that a DAC code and its output level move
      // together; the encoder's nearest-level search does not require it, but
      // a non-monotonic ladder is almost always a wiring mistake.
      for (uint32_t i = 1; i < n; ++i) {
        CHECK(d->levels[i] > d->levels[i - 1],
              "%s: level[%u]=%u is not above level[%u]=%u", d->name, i,
              d->levels[i], i - 1, d->levels[i - 1]);
      }
      CHECK(d->levels[n - 1] <= COMPOSITE_LEVEL_MAX,
            "%s: top level %u exceeds %u", d->name, d->levels[n - 1],
            COMPOSITE_LEVEL_MAX);
    }
    // The blanking and peak-white levels must be *exactly* representable.
    // This is why the 3-bit ladder uses a 1:2:5 conductance ratio rather than
    // a binary 1:2:4 — a binary ladder misses blanking by ~14%, which the
    // receiver's clamp reads as a black-level error.
    for (const CompositeTiming *t : ts) {
      const uint8_t wanted[] = {t->lvlSyncTip, t->lvlBlank, t->lvlWhite};
      const char *names[] = {"sync tip", "blanking", "peak white"};
      for (int k = 0; k < 3; ++k) {
        bool exact = (d->bits >= COMPOSITE_LEVEL_BITS);
        for (uint32_t i = 0; i < n && !exact; ++i) {
          if (d->levels[i] == wanted[k]) exact = true;
        }
        CHECK(exact, "%s/%s: %s (level %u) is not representable", t->name,
              d->name, names[k], wanted[k]);
      }
    }
    printf("  %-22s %u bits, GPIO%u..%u\n", d->name, d->bits, d->basePin,
           d->basePin + d->bits - 1);
  }
}

}  // namespace

int main() {
  printf("composite_encode tests\n\n");
  testDacProfiles();
  testGeometry();
  testTimingTables();
  testFieldStructure();
  testBurst();
  testLevels();
  testMonochrome();
  printf("\n%s (%d failure%s)\n", gFailures == 0 ? "PASS" : "FAIL", gFailures,
         gFailures == 1 ? "" : "s");
  return gFailures == 0 ? 0 : 1;
}
