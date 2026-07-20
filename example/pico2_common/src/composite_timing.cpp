#include "lcdtap/pico2/composite_timing.hpp"

namespace lcdtap::pico2 {

// 7-bit R-2R ladder (R = 1.0k rungs, 2R = 2.0k series, 3.9k shunt) buffered
// by an NPN emitter follower running off VSYS, then a 75 ohm series resistor.
// Full scale (code 127) is 1.30 V at a 75 ohm terminated load.
const CompositeDacProfile COMPOSITE_DAC_7BIT_GPIO5 = {
    .name = "7bit R-2R GPIO5-11",
    .bits = 7,
    .basePin = 5,
    .levels = {0},  // identity mapping; unused for a 7-bit DAC
};

// 3-bit weighted DAC: GPIO5 via 15k, GPIO6 via 7.5k, GPIO7 via 3.0k into a
// 4.3k shunt, feeding the same emitter-follower buffer as the 7-bit ladder.
// The conductance ratio is exactly 1:2:5 rather than the binary 1:2:4, which
// places the sync tip (0 V), blanking (0.286 V) and peak white (1.000 V)
// exactly on codes 0, 28 and 98 — the same normalized levels the timing
// tables use for lvlBlank and lvlWhite. A binary ladder would put blanking
// 14% off, which the receiver's clamp would read as a black-level error.
const CompositeDacProfile COMPOSITE_DAC_3BIT_GPIO5 = {
    .name = "3bit weighted GPIO5-7",
    .bits = 3,
    .basePin = 5,
    .levels = {0, 14, 28, 42, 70, 84, 98, 112},
};

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
    .pioClkdivInt = 22,

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
};

// PAL-B, 288p non-interlaced.
// 1135 samples/line at 17.735294 MHz = 63.9967 us.
//
// Note: fsc / fH is exactly 283.75 here, so the 25 Hz offset of broadcast PAL
// is not reproduced. Dot crawl is slightly more visible as a result; sync and
// colour lock are unaffected.
const CompositeTiming COMPOSITE_TIMING_PAL_B_288P = {
    .name = "PAL-B 288p",
    .colorEnabled = false,  // Phase 1 is monochrome; Phase 2 enables chroma
    .palSwitch = true,

    // 12 MHz / 2 * 201 = 1206 MHz VCO, / (2 * 2) = 301.500 MHz
    .clkSysKhz = 301500,
    .pllRefdiv = 2,
    .pllFbdiv = 201,
    .pllPostdiv1 = 2,
    .pllPostdiv2 = 2,
    .pioClkdivInt = 17,

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
};

}  // namespace lcdtap::pico2
