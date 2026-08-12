#pragma once

// Built-in test vectors and per-controller customization option tables.

#include <cstdint>

#include "lcdtap/config.hpp"

namespace testrig {

// KS0108 vectors only: write the pattern frame with the cs mask alternating
// per data byte (no commands in between), exercising the bare cs-change
// batch split in the target's spiSlaveProcess2Cs.
constexpr uint8_t VEC_FLAG_KS_INTERLEAVE = 0x01;

struct TestVector {
  const char* name;
  lcdtap::ConfigPreset preset;
  lcdtap::BusType busInterface;
  uint16_t buffWidth, buffHeight;           // 0 for character LCDs
  lcdtap::InterfaceFormat interfaceFormat;  // NUM_FORMATS for character LCDs
  uint8_t outputRot;
  lcdtap::TrimMode trimMode;
  uint16_t trimX, trimY, trimWidth, trimHeight;
  uint8_t flags;  // VEC_FLAG_*; existing rows default to 0
};

// Global bus clock speed classes: index into the per-family/bus frequency
// list (freqChoices). Applied to a whole test run from the title screen.
enum class SpeedClass : uint8_t { SLOW, MEDIUM, FAST, EXTRA, COUNT };
extern const char* const SPEED_CLASS_NAMES[];  // "Slow" ... "Extra"

// Bus clock for a vector under the given speed class.
uint32_t freqForClass(lcdtap::ControllerFamily fam, lcdtap::BusType bus,
                      SpeedClass cls);

// The vector table. UI index 0 is the "ALL" pseudo entry; vector i is
// TEST_VECTORS[i - 1].
extern const TestVector TEST_VECTORS[];
extern const int NUM_TEST_VECTORS;

// True for character-LCD (gettextbuffer) vectors.
bool vectorIsText(const TestVector& v);

// ---------------------------------------------------------------------------
// Customization option tables
// ---------------------------------------------------------------------------

// Allowed buses for a controller family. Returns count; fills out[].
int allowedBuses(lcdtap::ControllerFamily fam, lcdtap::BusType* out, int cap);

// Clock frequency choices per family and bus (4, ordered slow to fast =
// SpeedClass indices). Character LCDs (ST7032) get kHz-range parallel
// clocks: real HD44780-class modules cannot follow the graphic-panel rates.
int freqChoices(lcdtap::ControllerFamily fam, lcdtap::BusType bus,
                uint32_t* out, int cap);

// Framebuffer resolution choices per family. Returns count; w/h pairs.
int resolutionChoices(lcdtap::ControllerFamily fam, uint16_t* w, uint16_t* h,
                      int cap);

// Interface format choices per family.
int formatChoices(lcdtap::ControllerFamily fam, lcdtap::InterfaceFormat* out,
                  int cap);

// Controller family of a preset (mirror of getPresetConfig).
lcdtap::ControllerFamily presetFamily(lcdtap::ConfigPreset preset);

}  // namespace testrig
