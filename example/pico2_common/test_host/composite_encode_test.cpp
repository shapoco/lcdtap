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
  std::vector<uint16_t> samples;  // one physical DAC code per entry
  uint32_t transfersPerSlot = 0;
  uint32_t linesPerSlot = 0;
  // Physical codes, which differ per sink: the R-2R map is the identity but
  // the PWM map is not, so tests must compare against these rather than
  // against the normalized lvlXxx values.
  uint32_t codeSyncTip = 0;
  uint32_t codeBlank = 0;
  uint32_t codeWhite = 0;
  uint32_t codeMax = 0;
  uint32_t burstCode[2][COMPOSITE_NUM_PHASES] = {};
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
  std::vector<uint32_t> lut(compositeLutWords(t, dac));
  CompositeEncoder enc;
  if (!compositeEncoderInit(&enc, t, dac, lut.data())) {
    printf("  FAIL compositeEncoderInit(%s, %s) returned false\n", t->name,
           dac->name);
    gFailures++;
    return f;
  }
  f.transfersPerSlot = enc.transfersPerSlot;
  f.linesPerSlot = enc.linesPerSlot;
  f.codeSyncTip = enc.codeSyncTip;
  f.codeBlank = enc.codeBlank;
  f.codeWhite = enc.codeWhite;
  f.codeMax = enc.codeMax;
  for (uint32_t pol = 0; pol < 2; ++pol) {
    for (uint32_t i = 0; i < COMPOSITE_NUM_PHASES; ++i) {
      f.burstCode[pol][i] = enc.burstCode[pol][i];
    }
  }

  const std::vector<uint16_t> px = makeColorBars(t->hActivePixels);
  const uint32_t numSlots = t->vTotalLines / enc.linesPerSlot;

  CHECK(t->vTotalLines % enc.linesPerSlot == 0,
        "%s/%s: vTotalLines %u is not a multiple of linesPerSlot %u", t->name,
        dac->name, t->vTotalLines, enc.linesPerSlot);

  // Byte-granular so that both transfer formats share one buffer.
  std::vector<uint8_t> slot(enc.bytesPerSlot);
  for (uint32_t s = 0; s < numSlots; ++s) {
    CompositeSampleWriter w;
    const uint32_t firstLine = s * enc.linesPerSlot;
    // Mirror the driver: seed the writer with the slot's absolute first line so
    // the subcarrier phase is correct across slot boundaries.
    compositeWriterInit(&w, &enc, slot.data(), firstLine);
    for (uint32_t k = 0; k < enc.linesPerSlot; ++k) {
      uint32_t y = 0;
      const uint32_t line = firstLine + k;
      const CompositeLineType type = compositeClassifyLine(t, line, &y);
      compositeEmitLine(&w, &enc, type, line, px.data());
    }
    // The whole point of the group structure: a group always ends exactly on
    // a transfer boundary with nothing left in the accumulator.
    CHECK(w.nbits == 0, "%s/%s slot %u: %u bits left over", t->name, dac->name,
          s, w.nbits);
    const uint8_t *end = (const uint8_t *)w.dst.w;
    CHECK(end == slot.data() + enc.bytesPerSlot,
          "%s/%s slot %u: wrote %ld bytes, expected %u", t->name, dac->name, s,
          (long)(end - slot.data()), enc.bytesPerSlot);
    // sampleIndex is seeded with the slot's absolute offset, so it ends one
    // slot further along, not at linesPerSlot * samplesPerLine.
    const uint32_t wantIndex = (s + 1) * enc.linesPerSlot * t->samplesPerLine;
    CHECK(w.sampleIndex == wantIndex,
          "%s/%s slot %u: sampleIndex %u, expected %u", t->name, dac->name, s,
          w.sampleIndex, wantIndex);

    // Recover individual sample codes from the transfer format.
    if (dac->kind == CompositeDacKind::PWM) {
      const uint16_t *h = (const uint16_t *)slot.data();
      for (uint32_t i = 0; i < enc.transfersPerSlot; ++i) {
        f.samples.push_back(h[i]);
      }
    } else {
      const uint32_t *ww = (const uint32_t *)slot.data();
      const uint32_t mask = (1u << dac->bits) - 1u;
      for (uint32_t i = 0; i < enc.transfersPerSlot; ++i) {
        for (uint32_t j = 0; j < enc.samplesPerTransfer; ++j) {
          f.samples.push_back((ww[i] >> (j * dac->bits)) & mask);
        }
      }
    }
  }
  return f;
}

double usPerSample(const CompositeTiming *t) {
  const double sampleHz = (double)t->clkSysKhz * 1000.0 / t->clkPerSample;
  return 1e6 / sampleHz;
}

// --- structural checks -----------------------------------------------------

void testGeometry() {
  printf("geometry\n");
  struct Case {
    const CompositeTiming *t;
    const CompositeDacProfile *d;
    uint32_t linesPerSlot;
    uint32_t transfersPerSlot;
  };
  // The R-2R rows are the values from before the PWM sink existed; they must
  // not move, since the R-2R output has to stay bit-for-bit identical.
  const Case cases[] = {
      {&COMPOSITE_TIMING_NTSC_J_240P, &COMPOSITE_DAC_R2R_GPIO5, 2, 455},
      {&COMPOSITE_TIMING_PAL_B_288P, &COMPOSITE_DAC_R2R_GPIO5, 4, 1135},
      {&COMPOSITE_TIMING_NTSC_J_240P, &COMPOSITE_DAC_PWM_GPIO10, 2, 1820},
      {&COMPOSITE_TIMING_PAL_B_288P, &COMPOSITE_DAC_PWM_GPIO10, 2, 2270},
  };
  for (const Case &c : cases) {
    const uint32_t lps = compositeLinesPerSlot(c.t, c.d);
    const uint32_t tps = compositeTransfersPerSlot(c.t, c.d);
    CHECK(lps == c.linesPerSlot, "%s/%s linesPerSlot %u, expected %u",
          c.t->name, c.d->name, lps, c.linesPerSlot);
    CHECK(tps == c.transfersPerSlot, "%s/%s transfersPerSlot %u, expected %u",
          c.t->name, c.d->name, tps, c.transfersPerSlot);
    // The group must hold a whole number of both transfers and lines.
    const uint32_t spt =
        c.d->kind == CompositeDacKind::PWM ? 1u : 32u / c.d->bits;
    CHECK(tps * spt == lps * c.t->samplesPerLine,
          "%s/%s: %u transfers * %u != %u lines * %u samples", c.t->name,
          c.d->name, tps, spt, lps, c.t->samplesPerLine);
    // Core 1 must get the same fill budget regardless of sink.
    CHECK(lps >= COMPOSITE_MIN_LINES_PER_SLOT,
          "%s/%s: linesPerSlot %u below the floor %u", c.t->name, c.d->name,
          lps, COMPOSITE_MIN_LINES_PER_SLOT);
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
    const double sampleHz = (double)t->clkSysKhz * 1000.0 / t->clkPerSample;
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
  const CompositeDacProfile *ds[] = {&COMPOSITE_DAC_R2R_GPIO5,
                                     &COMPOSITE_DAC_PWM_GPIO10};
  for (const CompositeTiming *t : ts) {
    for (const CompositeDacProfile *d : ds) {
      const Field f = generateField(t, d);
      if (f.samples.empty()) continue;

      const uint32_t expect = t->vTotalLines * t->samplesPerLine;
      CHECK(f.samples.size() == expect,
            "%s/%s: field has %zu samples, expected %u", t->name, d->name,
            f.samples.size(), expect);

      // The subcarrier phase must be continuous across the field boundary,
      // which requires the field to be a whole number of subcarrier cycles.
      CHECK(expect % 4u == 0, "%s: field is %u samples, not a multiple of 4",
            t->name, expect);

      // Every line must begin with a sync tip and contain exactly one sync
      // interval of the expected width (equalizing/serration lines aside).
      const uint32_t activeStart = t->vTotalLines - t->vActiveLines;
      for (uint32_t line = activeStart; line < t->vTotalLines; ++line) {
        const uint16_t *s = &f.samples[line * t->samplesPerLine];
        for (uint32_t i = 0; i < t->hSyncWidth; ++i) {
          if (s[i] != f.codeSyncTip) {
            CHECK(false, "%s/%s line %u: sample %u is %u, expected sync tip %u",
                  t->name, d->name, line, i, s[i], f.codeSyncTip);
            break;
          }
        }
        CHECK(s[t->hSyncWidth] != f.codeSyncTip,
              "%s/%s line %u: sync tip extends past hSyncWidth %u", t->name,
              d->name, line, t->hSyncWidth);
      }
      if (d == ds[0]) {
        printf("  %-12s %zu samples, %.4f us sync, %.4f us line\n", t->name,
               f.samples.size(), t->hSyncWidth * usPerSample(t),
               t->samplesPerLine * usPerSample(t));
      }
    }
  }
}

void testBurst() {
  printf("colour burst\n");
  const CompositeTiming *ts[] = {&COMPOSITE_TIMING_NTSC_J_240P,
                                 &COMPOSITE_TIMING_PAL_B_288P};
  const CompositeDacProfile *ds[] = {&COMPOSITE_DAC_R2R_GPIO5,
                                     &COMPOSITE_DAC_PWM_GPIO10};
  for (const CompositeTiming *t : ts) {
    for (const CompositeDacProfile *d : ds) {
      const Field f = generateField(t, d);
      if (f.samples.empty()) continue;

      const uint32_t activeStart = t->vTotalLines - t->vActiveLines;
      bool ok = true;
      for (uint32_t line = activeStart; line < t->vTotalLines && ok; ++line) {
        const uint32_t base = line * t->samplesPerLine;
        // PAL swings the burst per line; the polarity follows the same V switch
        // (line parity) the encoder used. NTSC always uses polarity 0.
        const uint32_t pol = t->palSwitch ? (line & 1u) : 0u;
        const uint16_t *s = &f.samples[base + t->hBurstStart];
        for (uint32_t i = 0; i < t->hBurstCycles * 4u; ++i) {
          // Phase comes from the absolute sample index, so burst and chroma
          // share one reference across slots.
          const uint32_t phase = (base + t->hBurstStart + i) & 3u;
          if (s[i] != f.burstCode[pol][phase]) {
            CHECK(false,
                  "%s/%s line %u burst sample %u: %u, expected %u "
                  "(pol %u phase %u)",
                  t->name, d->name, line, i, s[i], f.burstCode[pol][phase], pol,
                  phase);
            ok = false;
            break;
          }
        }
      }
      // The burst must be suppressed through the vertical interval.
      for (uint32_t line = 0; line < t->vBurstBlankEnd; ++line) {
        const uint16_t *s =
            &f.samples[line * t->samplesPerLine + t->hBurstStart];
        bool modulated = false;
        for (uint32_t i = 0; i < t->hBurstCycles * 4u; ++i) {
          if (s[i] != f.codeBlank && s[i] != f.codeSyncTip) modulated = true;
        }
        CHECK(!modulated,
              "%s/%s line %u: burst present during vertical blanking", t->name,
              d->name, line);
      }
      // Amplitude in physical codes. The PWM sink quantizes this brutally, and
      // PAL's +-45 degree swing costs a further 1/sqrt(2), so print it as a
      // number rather than a surprise on the bench.
      const int amp = (int)f.burstCode[0][2] - (int)f.codeBlank;
      printf("  %-12s %-20s %u cycles at %.2f us, amplitude +-%d of %u..%u\n",
             t->name, d->name, t->hBurstCycles, t->hBurstStart * usPerSample(t),
             amp, f.codeBlank, f.codeWhite);
    }
  }
}

void testLevels() {
  printf("signal levels\n");
  const CompositeTiming *t = &COMPOSITE_TIMING_NTSC_J_240P;
  const CompositeDacProfile *ds[] = {&COMPOSITE_DAC_R2R_GPIO5,
                                     &COMPOSITE_DAC_PWM_GPIO10};
  for (const CompositeDacProfile *d : ds) {
    const Field f = generateField(t, d);
    if (f.samples.empty()) continue;

    // Nothing may exceed the DAC range, on any line.
    uint32_t lo = 0xFFFFu, hi = 0;
    for (uint16_t v : f.samples) {
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    CHECK(hi <= f.codeMax, "%s: peak code %u exceeds codeMax %u", d->name, hi,
          f.codeMax);
    CHECK(lo == f.codeSyncTip, "%s: minimum code is %u, expected sync tip %u",
          d->name, lo, f.codeSyncTip);
    // The top of the PWM range is where CC would exceed TOP, whose behaviour
    // is not documented in the SDK. Keep it unreachable.
    if (d->kind == CompositeDacKind::PWM) {
      CHECK(hi < f.codeMax, "%s: peak code %u reaches codeMax %u", d->name, hi,
            f.codeMax);
    }

    // White and black bars must land on the nominal codes: the leftmost bar
    // is white, the rightmost is black, and neither carries chroma.
    const uint32_t line = t->vTotalLines - 1;
    const uint16_t *a = &f.samples[line * t->samplesPerLine + t->hActiveStart];
    const uint32_t lastBar = t->hActiveSamples - 8;
    CHECK(a[8] == f.codeWhite, "%s: white bar reads %u, expected %u", d->name,
          a[8], f.codeWhite);
    CHECK(a[lastBar] == f.codeBlank, "%s: black bar reads %u, expected %u",
          d->name, a[lastBar], f.codeBlank);
    printf("  %-20s code range %u..%u (sync %u, blank %u, white %u, max %u)\n",
           d->name, lo, hi, f.codeSyncTip, f.codeBlank, f.codeWhite, f.codeMax);
  }
}

void testPalColor() {
  printf("PAL colour levels\n");
  // PAL is now colour: verify the active line carries chroma (some pixel's two
  // samples differ), the greyscale endpoints do not, and nothing exceeds the
  // DAC range. testLevels only covers NTSC, so this extends level coverage.
  const CompositeTiming *t = &COMPOSITE_TIMING_PAL_B_288P;
  const CompositeDacProfile *ds[] = {&COMPOSITE_DAC_R2R_GPIO5,
                                     &COMPOSITE_DAC_PWM_GPIO10};
  for (const CompositeDacProfile *d : ds) {
    const Field f = generateField(t, d);
    if (f.samples.empty()) continue;

    // Nothing may exceed the DAC range or dip below the sync tip.
    uint32_t lo = 0xFFFFu, hi = 0;
    for (uint16_t v : f.samples) {
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    CHECK(hi <= f.codeMax, "%s: peak code %u exceeds codeMax %u", d->name, hi,
          f.codeMax);
    CHECK(lo == f.codeSyncTip, "%s: minimum code is %u, expected sync tip %u",
          d->name, lo, f.codeSyncTip);
    if (d->kind == CompositeDacKind::PWM) {
      CHECK(hi < f.codeMax, "%s: peak code %u reaches codeMax %u", d->name, hi,
            f.codeMax);
    }

    const uint32_t line = t->vTotalLines - 1;
    const uint16_t *a = &f.samples[line * t->samplesPerLine + t->hActiveStart];
    // Colour bars must modulate: at least one pixel's two samples must differ.
    bool hasChroma = false;
    for (uint32_t i = 0; i < t->hActiveSamples; i += 2) {
      if (a[i] != a[i + 1]) hasChroma = true;
    }
    CHECK(hasChroma, "%s/%s: no chroma on a colour line", t->name, d->name);
    // The white and black end bars carry no chroma, so their pairs are flat.
    const uint32_t lastBar = t->hActiveSamples - 8;
    CHECK(a[8] == a[9] && a[8] == f.codeWhite,
          "%s: white bar reads %u/%u, expected flat %u", d->name, a[8], a[9],
          f.codeWhite);
    CHECK(a[lastBar] == a[lastBar + 1] && a[lastBar] == f.codeBlank,
          "%s: black bar reads %u/%u, expected flat %u", d->name, a[lastBar],
          a[lastBar + 1], f.codeBlank);
    printf("  %-20s chroma present, codes %u..%u within 0..%u\n", d->name, lo,
           hi, f.codeMax);
  }
}

// Demodulate the way a receiver does, and check the hue of each colour bar
// against the standard vectorscope angles.
//
// Every other chroma assertion in this file is blind to the sign of the
// quadrature axis: testBurst compares the encoder against its own burstCode,
// testLevels only samples the white and black bars, and testLevelMaps bounds
// excursion magnitude. A V-axis sign inversion therefore passed the whole
// suite and only showed up on a TV, as every hue mirrored about the U axis
// with luma untouched.
//
// The measurement is taken *relative to the -U reference axis*, because that
// is the only reference a receiver has -- which also means the absolute
// handedness of the basis used below cancels out and cannot bias the result.
// For PAL the burst swings +-45 degrees line to line, so the reference is
// recovered by averaging two adjacent lines' bursts (the swing cancels), and
// the transmitted V is de-switched per line parity before the hue is read.
//
// Several lines spanning multiple slots are checked. A per-slot subcarrier
// phase error (the writer seed) rotates a whole slot 180 degrees relative to
// the fixed reference, so both the burst-swing check and the hue check on the
// lines in that slot would fail -- which is the point.
//
// Honest limitation: the expected sense (delta = 180 - hue) was settled by
// looking at a real TV, not derived from first principles. This is a
// regression lock on verified behaviour, not a proof of standards compliance.
void testChromaPhase() {
  printf("chroma phase (demodulated against the burst)\n");

  // Standard vectorscope angles, atan2(V, U) with the burst at 180 degrees.
  struct Bar {
    const char *name;
    int index;  // position in makeColorBars
    double hue;
  };
  const Bar bars[] = {
      {"yellow", 1, 167.1}, {"cyan", 2, 283.5}, {"green", 3, 240.7},
      {"magenta", 4, 60.7}, {"red", 5, 103.5},  {"blue", 6, 347.1},
  };

  const CompositeTiming *ts[] = {&COMPOSITE_TIMING_NTSC_J_240P,
                                 &COMPOSITE_TIMING_PAL_B_288P};
  const CompositeDacProfile *ds[] = {&COMPOSITE_DAC_R2R_GPIO5,
                                     &COMPOSITE_DAC_PWM_GPIO10};

  for (const CompositeTiming *t : ts) {
    for (const CompositeDacProfile *d : ds) {
      const Field f = generateField(t, d);
      if (f.samples.empty()) continue;

      const uint32_t activeStart = t->vTotalLines - t->vActiveLines;

      // Correlate a sample run against the quadrature basis. `first` is the
      // absolute sample index so the phase reference is the same one the
      // encoder used.
      auto project = [&](uint32_t first, uint32_t count, double *outU,
                         double *outV) {
        double su = 0.0, sv = 0.0;
        for (uint32_t i = 0; i < count; ++i) {
          const double c = (double)f.samples[first + i] - (double)f.codeBlank;
          const uint32_t p = (first + i) & 3u;
          const double cosv[4] = {1, 0, -1, 0};
          const double sinv[4] = {0, 1, 0, -1};
          su += c * cosv[p];
          sv += c * sinv[p];
        }
        *outU = su * 2.0 / count;
        *outV = sv * 2.0 / count;
      };
      auto burstOf = [&](uint32_t line, double *bu, double *bv) {
        project(line * t->samplesPerLine + t->hBurstStart, t->hBurstCycles * 4u,
                bu, bv);
      };

      // Fixed -U reference. PAL averages an adjacent pair so the +-45 swing
      // cancels; NTSC has no swing, so one line is enough.
      double ru, rv, u1, v1;
      burstOf(activeStart, &ru, &rv);
      if (t->palSwitch) {
        burstOf(activeStart + 1u, &u1, &v1);
        ru += u1;
        rv += v1;
      }
      const double refAngle = atan2(rv, ru) * 180.0 / M_PI;
      CHECK(sqrt(ru * ru + rv * rv) > 0.5, "%s/%s: no burst (amplitude %.2f)",
            t->name, d->name, sqrt(ru * ru + rv * rv));

      // Lines spanning several slots, including a slot boundary.
      const uint32_t lines[] = {activeStart,         activeStart + 1u,
                                activeStart + 2u,    activeStart + 3u,
                                t->vTotalLines - 2u, t->vTotalLines - 1u};
      int barsOk = 0, barsTotal = 0;
      for (uint32_t line : lines) {
        const uint32_t base = line * t->samplesPerLine;
        const uint32_t pol = t->palSwitch ? (line & 1u) : 0u;

        // The burst must sit on the reference (NTSC) or swing +45 on even /
        // -45 on odd lines (PAL).
        double bu, bv;
        burstOf(line, &bu, &bv);
        double swing = atan2(bv, bu) * 180.0 / M_PI - refAngle;
        swing = fmod(swing + 540.0, 360.0) - 180.0;  // -180..180
        const double wantSwing = t->palSwitch ? (pol ? -45.0 : 45.0) : 0.0;
        double swingErr = fabs(swing - wantSwing);
        if (swingErr > 180.0) swingErr = 360.0 - swingErr;
        CHECK(swingErr < 15.0,
              "%s/%s line %u: burst swing %.1f deg, expected %.1f", t->name,
              d->name, line, swing, wantSwing);

        for (const Bar &b : bars) {
          // Sample the middle of the bar to stay clear of the transitions.
          const uint32_t barW = t->hActiveSamples / 8u;
          const uint32_t start = t->hActiveStart + b.index * barW + barW / 4u;
          const uint32_t count = (barW / 2u) & ~3u;  // whole subcarrier cycles

          double cu, cv;
          project(base + start, count, &cu, &cv);
          // De-switch the PAL V axis so the hue reads the same on both lines.
          if (pol) cv = -cv;
          const double amp = sqrt(cu * cu + cv * cv);
          double delta = atan2(cv, cu) * 180.0 / M_PI - refAngle;
          delta = fmod(delta + 720.0, 360.0);

          // A colour of hue h sits at (180 - h) from the reference.
          double want = fmod(180.0 - b.hue + 720.0, 360.0);
          double err = fabs(delta - want);
          if (err > 180.0) err = 360.0 - err;

          barsTotal++;
          CHECK(err < 12.0,
                "%s/%s line %u %s: hue is %.1f deg, expected %.1f "
                "(off by %.1f -- mirrored reads as %.1f)",
                t->name, d->name, line, b.name, delta, want, err,
                fmod(360.0 - delta, 360.0));
          CHECK(amp > 0.5, "%s/%s line %u %s: no chroma (amplitude %.2f)",
                t->name, d->name, line, b.name, amp);
          if (err < 12.0 && amp > 0.5) barsOk++;
        }
      }
      printf("  %-12s %-20s ref %.1f deg, %d/%d bar reads on target\n", t->name,
             d->name, refAngle, barsOk, barsTotal);
    }
  }
}

void testLevelMaps() {
  printf("level maps\n");
  const CompositeTiming *ts[] = {&COMPOSITE_TIMING_NTSC_J_240P,
                                 &COMPOSITE_TIMING_PAL_B_288P};
  const CompositeDacProfile *ds[] = {&COMPOSITE_DAC_R2R_GPIO5,
                                     &COMPOSITE_DAC_PWM_GPIO10};

  // The R-2R map must be the exact identity, so that adding the PWM sink
  // cannot have perturbed the existing output.
  {
    const CompositeLevelMap m = compositeLevelMap(&COMPOSITE_TIMING_NTSC_J_240P,
                                                  &COMPOSITE_DAC_R2R_GPIO5);
    for (int32_t l = 0; l <= (int32_t)COMPOSITE_LEVEL_MAX; ++l) {
      const uint32_t c = compositeLevelToCode(m, l);
      if (c != (uint32_t)l) {
        CHECK(false, "R-2R map is not the identity: level %d -> code %u", l, c);
        break;
      }
    }
  }

  for (const CompositeTiming *t : ts) {
    for (const CompositeDacProfile *d : ds) {
      const CompositeLevelMap m = compositeLevelMap(t, d);
      const uint32_t sync = compositeLevelToCode(m, t->lvlSyncTip);
      const uint32_t blank = compositeLevelToCode(m, t->lvlBlank);
      const uint32_t white = compositeLevelToCode(m, t->lvlWhite);

      CHECK(sync == 0, "%s/%s: sync tip maps to %u, expected 0", t->name,
            d->name, sync);
      CHECK(blank > 0 && blank < white && white <= m.codeMax,
            "%s/%s: levels not ordered (sync %u, blank %u, white %u, max %u)",
            t->name, d->name, sync, blank, white, m.codeMax);

      // Blanking sits at a fixed fraction of white (0.286 / 1.000). A coarse
      // DAC cannot hit it exactly; bound how far off it may land, since the
      // receiver clamps on the back porch and reads the error as a black
      // level shift.
      const double want = (double)t->lvlWhite / t->lvlBlank;
      const double got = (double)white / blank;
      const double err = std::fabs(got - want) / want;
      CHECK(err < 0.06, "%s/%s: blank/white ratio off by %.1f%% (%u : %u)",
            t->name, d->name, err * 100.0, blank, white);

      // Chroma overshoots white by +133 IRE and undershoots blanking by
      // -33 IRE, so both excursions must fit the code range. This is the
      // constraint that the pwmCcWhite search was solving; asserting it here
      // means a stale value cannot survive a build.
      if (t->colorEnabled) {
        const double span = (double)white - blank;
        const double peak = blank + 1.33 * span;
        const double trough = blank - 0.33 * span;
        CHECK(peak <= m.codeMax,
              "%s/%s: peak chroma %.1f exceeds codeMax %u (lower pwmCcWhite)",
              t->name, d->name, peak, m.codeMax);
        CHECK(trough >= 0.0, "%s/%s: chroma trough %.1f below 0", t->name,
              d->name, trough);
      }

      printf("  %-12s %-20s sync %u blank %u white %u max %u (ratio %+.1f%%)\n",
             t->name, d->name, sync, blank, white, m.codeMax,
             (got / want - 1.0) * 100.0);
    }
  }
}

}  // namespace

int main() {
  printf("composite_encode tests\n\n");
  testLevelMaps();
  testChromaPhase();
  testGeometry();
  testTimingTables();
  testFieldStructure();
  testBurst();
  testLevels();
  testPalColor();
  printf("\n%s (%d failure%s)\n", gFailures == 0 ? "PASS" : "FAIL", gFailures,
         gFailures == 1 ? "" : "s");
  return gFailures == 0 ? 0 : 1;
}
