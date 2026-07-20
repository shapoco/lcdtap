#pragma once

// Host-side video output selection.
//
// This is deliberately *not* a lcdtap::ConfigId: the LcdTap library has no
// concept of how the host gets pixels onto a screen, and adding one would
// mean modifying lib/. The value lives in ConfigFile and is surfaced through
// an OSD custom item and a dedicated UART parameter.

#include <cstdint>

#include "lcdtap/lcdtap.hpp"

enum class OutputInterface : uint8_t {
  DVI_D = 0,
  NTSC = 1,
  PAL = 2,
};

static constexpr uint8_t OUTPUT_INTERFACE_COUNT = 3u;

// Number of host-side settings that sit alongside the library's ConfigId
// list: outputInterface and compositeDac.
static constexpr int NUM_HOST_PARAMS = 2;

// Both the OSD menu (onOsdMenuOpen in main.cpp) and the UART parameter list
// (buildParamChunk in uart_intf.cpp) place the host-side settings immediately
// before this library setting, so the two orders stay in step. Change it here
// and both follow.
static constexpr lcdtap::ConfigId HOST_PARAM_ANCHOR =
    lcdtap::ConfigId::OUTPUT_ROT;

// Static storage duration is required: ConfigEntry::options holds this
// pointer and formatConfigValue() dereferences it on every OSD render.
static const char *OUTPUT_INTERFACE_NAMES[] = {"DVI-D", "NTSC", "PAL"};

inline bool outputInterfaceIsComposite(OutputInterface v) {
  return v == OutputInterface::NTSC || v == OutputInterface::PAL;
}

// Composite output drives GPIO5-11, which in PARALLEL bus mode are data and
// D/C# lines driven by the external host controller. Selecting it there would
// be an output-vs-output conflict, so it is forbidden rather than merely
// unsupported.
inline bool outputInterfaceAllowed(OutputInterface v, lcdtap::BusType bus) {
  if (!outputInterfaceIsComposite(v)) return true;
  return bus != lcdtap::BusType::PARALLEL;
}

// Clamp a value that may have come from flash, a UART client, or an OSD item
// that was greyed out while its stale value was still selected.
inline OutputInterface outputInterfaceSanitize(OutputInterface v,
                                               lcdtap::BusType bus) {
  if (static_cast<uint8_t>(v) >= OUTPUT_INTERFACE_COUNT) {
    return OutputInterface::DVI_D;
  }
  return outputInterfaceAllowed(v, bus) ? v : OutputInterface::DVI_D;
}
