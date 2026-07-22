#include "lcdtap/pico2/composite_timing.hpp"

namespace lcdtap::pico2 {

// 7-bit R-2R ladder driven straight into the 75 ohm line, no buffer.
// R = 100 ohm series between nodes, 2R = 160 ohm rungs to each GPIO, 200 ohm
// far-end termination. The rung is 160 not 200 because each pad adds ~40 ohm
// at the 12 mA drive setting, so 160 + 40 ~= 200 = 2R and the binary ratio
// holds (otherwise mid-grey goes non-monotonic). GPIO5 is the LSB and GPIO11
// the MSB, so the output is tapped at the GPIO11 end into the RCA centre.
// See README for the diagram and bench-trimming notes. Levels land ~8% high;
// trim them with the resistors, not with lvlWhite/lvlBlank, which the PWM
// sink shares.
const CompositeDacProfile COMPOSITE_DAC_R2R_GPIO5 = {
    .name = "R-2R 7bit GPIO5-11",
    .kind = CompositeDacKind::R2R_7BIT,
    .bits = 7,
    .basePin = 5,
};

// Single GPIO through a series resistor into a shunt capacitor, no buffer.
// R = 150 ohm gives about 12 mA peak and roughly 70% of the standard
// amplitude; drop to 120 ohm if a receiver refuses to lock. C = 100..470 pF
// for a corner around 5-6 MHz. Nominal values; trim on the bench.
const CompositeDacProfile COMPOSITE_DAC_PWM_GPIO10 = {
    .name = "PWM GPIO10",
    .kind = CompositeDacKind::PWM,
    .bits = 0,
    .basePin = 10,
};

CompositeLevelMap compositeLevelMap(const CompositeTiming *timing,
                                    const CompositeDacProfile *dac) {
  CompositeLevelMap m;
  m.levelWhite = timing->lvlWhite;
  if (dac->kind == CompositeDacKind::PWM) {
    m.codeWhite = timing->pwmCcWhite;
    // The PWM period is clkPerSample counts, so the compare value can span
    // 0..clkPerSample. The level table never reaches the top of that range
    // (peak chroma is 21 of 22 on NTSC), which is deliberate: the behaviour
    // of CC > TOP is not documented in the SDK headers, so nothing here
    // depends on it.
    m.codeMax = timing->clkPerSample;
  } else {
    // R-2R: the normalized space is the code space, so this is the identity.
    m.codeWhite = timing->lvlWhite;
    m.codeMax = COMPOSITE_LEVEL_MAX;
  }
  return m;
}

// NTSC-J (0 IRE setup, black == blanking), 240p non-interlaced.
// 910 samples/line at 14.318182 MHz = 63.5556 us exactly.
const CompositeTiming COMPOSITE_TIMING_NTSC_J_240P = {
    .name = "NTSC-J 240p",
    .colorEnabled = true,
    .palSwitch = false,

    // 12 MHz / 1 * 105 = 1260 MHz VCO, / (2 * 2) = 315.000 MHz
    .clkSysKhz = 315000,
    .pllRefdiv = 1,
    .pllFbdiv = 105,
    .pllPostdiv1 = 2,
    .pllPostdiv2 = 2,
    .clkPerSample = 22,

    .samplesPerLine = 910,
    .hSyncWidth = 67,       // 4.7 us
    .hBurstStart = 76,      // 5.3 us from the leading edge of sync
    .hBurstCycles = 9,      // 36 samples, ending at 112
    .hActiveStart = 136,    // 9.5 us; standard window is 135..889
    .hActiveSamples = 752,  // trimmed from 754 to keep the pixel count even
    .hActivePixels = 376,

    .eqPulseWidth = 33,    // 2.3 us
    .serrPulseWidth = 67,  // 4.7 us high; sync tip is halfLine - 67
    .halfLineSamplesA = 455,
    .halfLineSamplesB = 455,

    .vPreEqLines = 3,
    .vSerrLines = 3,
    .vPostEqLines = 3,
    .vActiveLines = 240,  // active video starts on line 22
    .vTotalLines = 262,
    .vBurstBlankEnd = 9,

    .lvlSyncTip = 0,       // 0.000 V
    .lvlBlank = 28,        // 0.286 V
    .lvlBlack = 28,        // NTSC-J: black == blanking
    .lvlWhite = 98,        // 1.000 V
    .burstAmplitude = 14,  // +-20 IRE
    .chromaGain = 192,     // 0.75; raise until saturated bars just clip

    // PWM: 12 luma steps of the 22 available. 17 is the largest value that
    // still fits peak chroma (5 + 1.33*12 = 21.0 <= 22); 18 overflows at
    // 22.3. Blanking lands on 5, so the blank/white ratio is 2.9% off -- the
    // price of the extra amplitude, and well worth it here.
    .pwmCcWhite = 17,
};

// PAL-B, 288p non-interlaced.
// 1135 samples/line at 17.735294 MHz = 63.9967 us.
//
// Note: fsc / fH is exactly 283.75 here, so the 25 Hz offset of broadcast PAL
// is not reproduced. Dot crawl is slightly more visible as a result; sync and
// colour lock are unaffected.
const CompositeTiming COMPOSITE_TIMING_PAL_B_288P = {
    .name = "PAL-B 288p",
    .colorEnabled =
        true,  // chroma with the line-alternating V axis (palSwitch)
    .palSwitch = true,

    // 12 MHz / 2 * 201 = 1206 MHz VCO, / (2 * 2) = 301.500 MHz
    .clkSysKhz = 301500,
    .pllRefdiv = 2,
    .pllFbdiv = 201,
    .pllPostdiv1 = 2,
    .pllPostdiv2 = 2,
    .clkPerSample = 17,

    .samplesPerLine = 1135,
    .hSyncWidth = 83,     // 4.7 us
    .hBurstStart = 99,    // 5.6 us from the leading edge of sync
    .hBurstCycles = 10,   // 40 samples, ending at 139
    .hActiveStart = 186,  // 10.5 us
    .hActiveSamples = 920,
    .hActivePixels = 460,

    .eqPulseWidth = 42,    // 2.35 us
    .serrPulseWidth = 83,  // 4.7 us high
    // 1135 is odd, so the two halves of a line differ by one sample.
    .halfLineSamplesA = 567,
    .halfLineSamplesB = 568,

    .vPreEqLines = 5,
    .vSerrLines = 5,
    .vPostEqLines = 5,
    .vActiveLines = 288,  // active video starts on line 24
    .vTotalLines = 312,
    .vBurstBlankEnd = 15,

    .lvlSyncTip = 0,       // 0.000 V
    .lvlBlank = 28,        // 0.286 V
    .lvlBlack = 28,        // PAL: black == blanking
    .lvlWhite = 98,        // 1.000 V
    .burstAmplitude = 15,  // 300 mV peak-to-peak
    .chromaGain = 192,

    // PWM: 11 luma steps of the 17 available, chosen so peak chroma fits.
    // Colour overshoots white by +133 IRE: blank maps to 3, span is 8, so
    // peak = 3 + 1.33*8 = 13.6 <= 17. A larger value (e.g. 14, which gives an
    // exact blank/white ratio) overflows the ceiling at 17.3 the moment
    // colorEnabled is true. testLevelMaps() fails the build if this and
    // colorEnabled drift apart.
    .pwmCcWhite = 11,
};

}  // namespace lcdtap::pico2
