#pragma once

// Which composite DAC to drive, when the output interface is NTSC or PAL.
//
// Kept separate from OutputInterface because the two are independent axes:
// the standard picks the clock and line timing, the DAC picks the pins and
// the peripheral. Splitting them also lets the OSD gate them separately,
// which a single flat list of five options could not express -- the library
// supports one enableKeyId range per item.

#include <cstdint>

#include "lcdtap/lcdtap.hpp"

enum class CompositeDacKind : uint8_t {
  // One GPIO plus an RC filter. Simple and low quality: roughly 12 luma
  // steps and about 70% of the standard amplitude. Works on SPI and I2C.
  PWM = 0,
  // 7-bit R-2R ladder plus an emitter follower. Complex and high quality.
  // SPI only: the ladder spans GPIO5-11, which covers the I2C pins.
  R2R = 1,
};

static constexpr uint8_t COMPOSITE_DAC_KIND_COUNT = 2u;

// Static storage duration is required: ConfigEntry::options holds this
// pointer and formatConfigValue() dereferences it on every OSD render.
static const char *COMPOSITE_DAC_KIND_NAMES[] = {"PWM (GPIO10)",
                                                 "R-2R (GPIO5-11)"};

// The R-2R ladder occupies GPIO5-11, so it collides with the I2C bus on
// GPIO8/9. The PWM pin (GPIO10) is free on both SPI and I2C.
inline bool compositeDacAllowed(CompositeDacKind v, lcdtap::BusType bus) {
  if (v != CompositeDacKind::R2R) return true;
  return bus == lcdtap::BusType::SPI_4LINE || bus == lcdtap::BusType::SPI_3LINE;
}

// Clamp a value that may have come from flash, a UART client, or an OSD item
// that was greyed out while a now-illegal value was still selected.
// PWM is the fallback because it is legal on every bus that supports
// composite at all.
inline CompositeDacKind compositeDacSanitize(CompositeDacKind v,
                                             lcdtap::BusType bus) {
  if (static_cast<uint8_t>(v) >= COMPOSITE_DAC_KIND_COUNT) {
    return CompositeDacKind::PWM;
  }
  return compositeDacAllowed(v, bus) ? v : CompositeDacKind::PWM;
}
