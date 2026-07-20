#pragma once

#include <cstdint>

#include "lcdtap/lcdtap.hpp"

// Growing this struct changes sizeof(ConfigFile), so configs written by an
// older build fail the CRC check in loadConfig() and are discarded. That is
// intentional and accepted: the device falls back to defaults.
//
// Taking a byte from reserved[] instead keeps sizeof unchanged, so older
// configs still pass CRC and are reinterpreted rather than discarded. That
// only works because reserved[] was written as zero, so the new field must
// have a sane meaning at 0 -- CompositeDacKind::PWM is 0 for this reason.
struct ConfigFile {
  lcdtap::LcdTapConfig libConfig;
  uint8_t outputInterface;  // OutputInterface
  uint8_t compositeDac;     // CompositeDacKind; was reserved[0]
  uint8_t reserved[2];      // keep 4-byte granularity, room to grow
};

bool loadConfig(ConfigFile* out);
void saveConfig(const ConfigFile& cfg);
