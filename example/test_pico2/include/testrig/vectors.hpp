#pragma once

// Built-in test vectors and per-controller customization option tables.

#include <cstdint>

#include "lcdtap/config.hpp"

namespace testrig {

struct TestVector {
  const char* name;
  lcdtap::ConfigPreset preset;
  lcdtap::BusType busInterface;
  uint32_t busFreqHz;
  uint16_t buffWidth, buffHeight;           // 0 for character LCDs
  lcdtap::InterfaceFormat interfaceFormat;  // NUM_FORMATS for character LCDs
  uint8_t outputRot;
  lcdtap::TrimMode trimMode;
  uint16_t trimX, trimY, trimWidth, trimHeight;
};

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

// Clock frequency choices per bus (4 each; index 2 = "Fast" default).
int freqChoices(lcdtap::BusType bus, uint32_t* out, int cap);
uint32_t defaultFreq(lcdtap::BusType bus);

// Framebuffer resolution choices per family. Returns count; w/h pairs.
int resolutionChoices(lcdtap::ControllerFamily fam, uint16_t* w, uint16_t* h,
                      int cap);

// Interface format choices per family.
int formatChoices(lcdtap::ControllerFamily fam, lcdtap::InterfaceFormat* out,
                  int cap);

// Controller family of a preset (mirror of getPresetConfig).
lcdtap::ControllerFamily presetFamily(lcdtap::ConfigPreset preset);

}  // namespace testrig
