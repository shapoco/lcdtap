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
// code 127 corresponds to 1.30 V at a 75 ohm terminated load. Boards whose
// DAC is narrower than 7 bits map this space through a CompositeDacProfile.
static constexpr uint32_t COMPOSITE_LEVEL_BITS = 7u;
static constexpr uint32_t COMPOSITE_LEVEL_MAX = 127u;

struct CompositeTiming {
  // --- identity ---
  const char *name;
  bool colorEnabled;  // false = monochrome (no burst, no chroma)
  bool palSwitch;     // alternate the V component per line (PAL only)

  // --- clocks ---
  uint32_t clkSysKhz;     // system clock this timing requires
  uint32_t pllRefdiv;     // pll_sys recipe for clkSysKhz
  uint32_t pllFbdiv;      //
  uint32_t pllPostdiv1;   //
  uint32_t pllPostdiv2;   //
  uint16_t pioClkdivInt;  // clk_sys / sample rate; always an exact integer

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
};

// Maps the normalized 7-bit level space onto the physical DAC. For a 7-bit
// ladder the mapping is the identity and levels[] is unused.
struct CompositeDacProfile {
  const char *name;
  uint8_t bits;     // DAC width: 7 (SPI mode) or 3 (I2C mode)
  uint8_t basePin;  // GPIO of D0; the DAC occupies basePin .. basePin+bits-1
  // For bits < 7: the normalized level produced by each DAC code, ascending.
  // Only the first (1 << bits) entries are meaningful.
  uint8_t levels[8];
};

// 7-bit R-2R ladder on GPIO5..GPIO11, buffered by an emitter follower.
// Used in SPI slave modes, where GPIO5-11 are all free.
extern const CompositeDacProfile COMPOSITE_DAC_7BIT_GPIO5;

// 3-bit weighted DAC on GPIO5..GPIO7 (1.1k / 560 / 240 ohm), unbuffered,
// relying on the receiver's 75 ohm termination as the shunt leg. Used in I2C
// slave mode, where the bus occupies GPIO8/9 but GPIO5-7 are free.
// Levels: 0 / .15 / .30 / .45 / .70 / .85 / 1.00 / 1.15 V.
extern const CompositeDacProfile COMPOSITE_DAC_3BIT_GPIO5;

extern const CompositeTiming COMPOSITE_TIMING_NTSC_J_240P;
extern const CompositeTiming COMPOSITE_TIMING_PAL_B_288P;

}  // namespace lcdtap::pico2
