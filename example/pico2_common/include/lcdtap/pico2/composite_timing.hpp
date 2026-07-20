#pragma once

// Composite video (NTSC/PAL) timing tables.
//
// All horizontal quantities are expressed in DAC samples so that the line
// generator performs no arithmetic at run time. The sample rate is always
// 4x the colour subcarrier frequency, produced by an integer PIO divider:
//
//   NTSC : clk_sys = 315.000 MHz / 22 = 14.318182 MHz   (fsc = 315/88 MHz,
//                                                        exact, 0 ppm)
//   PAL  : clk_sys = 301.500 MHz / 17 = 17.735294 MHz   (+46 ppm of 4x fsc)
//
// Because burst and chroma are generated on the same sample grid they are
// perfectly coherent; the PAL frequency error only means the receiver locks
// its colour reference 205 Hz off nominal, far inside any burst PLL's range.
//
// Vertical quantities are in lines. Both standards are generated as
// non-interlaced 240p/288p, so no half-line field offsets are needed.

#include <cstdint>

namespace lcdtap::pico2 {

// Number of distinct subcarrier phases on a 4x fsc sample grid.
static constexpr uint32_t COMPOSITE_NUM_PHASES = 4u;

// Signal levels are held in a normalized 7-bit code space (0..127) where
// code 127 corresponds to 1.30 V at a 75 ohm terminated load. A DAC with
// fewer levels maps this space through a CompositeLevelMap.
static constexpr uint32_t COMPOSITE_LEVEL_BITS = 7u;
static constexpr uint32_t COMPOSITE_LEVEL_MAX = 127u;

// Which physical DAC drives the composite output.
enum class CompositeDacKind : uint8_t {
  // One GPIO driven by a PWM slice through an RC filter. Simple and low
  // quality: the number of duty steps equals the sample period in system
  // clocks, so only ~12 luma levels survive.
  PWM = 0,
  // 7-bit R-2R ladder plus an emitter follower. Complex and high quality.
  R2R_7BIT = 1,
};

struct CompositeTiming {
  // --- identity ---
  const char *name;
  bool colorEnabled;  // false = monochrome (no burst, no chroma)
  bool palSwitch;     // alternate the V component per line (PAL only)

  // --- clocks ---
  uint32_t clkSysKhz;    // system clock this timing requires
  uint32_t pllRefdiv;    // pll_sys recipe for clkSysKhz
  uint32_t pllFbdiv;     //
  uint32_t pllPostdiv1;  //
  uint32_t pllPostdiv2;  //
  // System clocks per sample; always an exact integer. Doubles as the PIO
  // clock divider (R-2R) and as the PWM period, so for the PWM sink the
  // number of duty steps is exactly this value.
  uint16_t clkPerSample;

  // --- horizontal, in samples ---
  uint16_t samplesPerLine;
  uint16_t hSyncWidth;      // sync tip, from sample 0
  uint16_t hBurstStart;     // first burst sample (after the breezeway)
  uint16_t hBurstCycles;    // burst length in subcarrier cycles (0 = no burst)
  uint16_t hActiveStart;    // first active-video sample
  uint16_t hActiveSamples;  // always 2 * hActivePixels
  uint16_t hActivePixels;   // framebuffer width fed to LcdTap; must be even

  // --- vertical blanking pulse shapes, in samples ---
  uint16_t eqPulseWidth;      // equalizing pulse (narrow) sync-tip width
  uint16_t serrPulseWidth;    // serration pulse: the *high* portion of a half
                              // line; the sync-tip portion is halfLine - this
  uint16_t halfLineSamplesA;  // first half of a line
  uint16_t halfLineSamplesB;  // second half; differs from A when
                              // samplesPerLine is odd (PAL: 567 / 568)

  // --- vertical structure, in lines, counted from the top of the field ---
  uint16_t vPreEqLines;
  uint16_t vSerrLines;
  uint16_t vPostEqLines;
  uint16_t vActiveLines;
  uint16_t vTotalLines;
  uint16_t vBurstBlankEnd;  // burst suppressed on lines [0, this)

  // --- signal levels, in the normalized 7-bit code space ---
  uint8_t lvlSyncTip;
  uint8_t lvlBlank;
  uint8_t lvlBlack;  // equals lvlBlank for NTSC-J and PAL (0 IRE setup)
  uint8_t lvlWhite;
  uint8_t burstAmplitude;  // peak deviation from lvlBlank
  uint8_t chromaGain;      // saturation scale, 256 = unity; tune on the bench

  // PWM sink only: the duty step that represents lvlWhite. White cannot sit
  // at 100% duty because chroma overshoots it by up to +133 IRE, so this is
  // chosen as the largest value whose chroma excursions still fit in
  // 0..clkPerSample. MUST be re-checked whenever colorEnabled changes --
  // enabling PAL colour requires dropping this from 14 to 11. The host test
  // testLevelMaps() enforces that coupling.
  uint8_t pwmCcWhite;
};

// Physical description of a DAC. Purely hardware; the level mapping lives in
// CompositeLevelMap because it also depends on the video standard.
struct CompositeDacProfile {
  const char *name;
  CompositeDacKind kind;
  uint8_t bits;     // PIO out-pin width; 0 for PWM
  uint8_t basePin;  // GPIO of D0 (R-2R) or the PWM output pin
};

// 7-bit R-2R ladder on GPIO5..GPIO11, buffered by an emitter follower.
// SPI slave modes only: GPIO8/9 belong to the I2C bus.
extern const CompositeDacProfile COMPOSITE_DAC_R2R_GPIO5;

// Single GPIO driven by a PWM slice through an RC filter. Usable on both SPI
// and I2C, since GPIO10 is free in either. Not usable on the parallel bus,
// where GPIO10 is data line D[7].
extern const CompositeDacProfile COMPOSITE_DAC_PWM_GPIO10;

// Maps the normalized 7-bit level space onto physical DAC codes:
//
//   code = min(codeMax, (level * codeWhite + lvlWhite/2) / lvlWhite)
//
// For the R-2R ladder codeWhite == lvlWhite, so the mapping is the identity.
// Resolved once at init; never used on a hot path.
struct CompositeLevelMap {
  uint16_t levelWhite;  // normalized level being mapped (timing->lvlWhite)
  uint16_t codeWhite;   // physical code representing it
  uint16_t codeMax;     // clamp ceiling (highest code the DAC can emit)
};

CompositeLevelMap compositeLevelMap(const CompositeTiming *timing,
                                    const CompositeDacProfile *dac);

extern const CompositeTiming COMPOSITE_TIMING_NTSC_J_240P;
extern const CompositeTiming COMPOSITE_TIMING_PAL_B_288P;

}  // namespace lcdtap::pico2
