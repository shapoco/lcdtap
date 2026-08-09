#pragma once

// PIO-USB host stack (core 1) facing the target device.
//
// The whole TinyUSB host stack runs on core 1 (the PIO-USB SOF interrupt
// must live on the core that called tuh_init). Core 0 talks to the target's
// CDC through lock-free byte rings only.

#include <cstdint>

namespace testrig {

// Launches core 1 (host stack + pumps). Call once after clock setup.
void usbHostStart();

// Target state, updated from core 1 mount callbacks.
bool usbHostCdcMounted();
bool usbHostMscMounted();  // target is in BOOTSEL / download mode

// True once the host stack has claimed its pio0/pio1 resources; gate any
// other free-PIO-resource consumer (CYW43) on this.
bool usbHostReady();

// Park core 1 in SRAM (IRQs off) around a flash erase/program on core 0.
// The target USB link may glitch for the duration; only save settings while
// no test run is in flight.
void usbHostFlashAcquire();
void usbHostFlashRelease();

// CDC stream to/from the target (core 0 side of the rings).
uint32_t usbHostCdcWrite(const uint8_t* data, uint32_t len);
uint32_t usbHostCdcRead(uint8_t* data, uint32_t len);
void usbHostCdcDrainInput();  // drop pending target->rig bytes

}  // namespace testrig
