#pragma once

// Phase 2: USB MSC passthrough. While the target sits in BOOTSEL, its
// RPI-RP2 drive is exposed to the PC so UF2 firmware can be written through
// the rig. Ported from mimicusb (msd.cpp). Compiled out when
// TESTRIG_MSC_PASSTHROUGH is 0 — the hooks below become no-ops and the MSC
// interface disappears from the device descriptors.

#include <cstdint>

#ifndef TESTRIG_MSC_PASSTHROUGH
#define TESTRIG_MSC_PASSTHROUGH 0
#endif

namespace testrig {

#if TESTRIG_MSC_PASSTHROUGH

// Called from the tuh_msc mount callbacks in usb_host.cpp (core 1).
void mscBridgeOnMount(uint8_t devAddr);
void mscBridgeOnUnmount(uint8_t devAddr);

// Core 1 loop hook: issues the pending block transfer to the target.
void mscBridgeHostTask();

#else

inline void mscBridgeOnMount(uint8_t) {}
inline void mscBridgeOnUnmount(uint8_t) {}
inline void mscBridgeHostTask() {}

#endif

}  // namespace testrig
