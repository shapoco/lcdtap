#pragma once

// Status LED driver (Pico 2 W onboard LED via CYW43 WL_GPIO0).
//
// Blink patterns follow the WiFi state; when connected the LED acts as an
// access lamp: steady ON with a short OFF pulse per API/CDC activity event.

#include <cstdint>

enum class LedPattern : uint8_t {
  UNCONFIGURED,  // 0.25 s ON / 0.75 s OFF
  CONNECTING,    // 0.25 s ON / 0.25 s OFF
  FAILED,        // 0.5 s ON / 0.5 s OFF
  CONNECTED,     // steady ON + activity pulses
};

void ledStatusSetPattern(LedPattern p);

// Note one activity event (HTTP accept/rx/tx, CDC command). Rate-limited
// internally; only meaningful in the CONNECTED pattern.
void ledActivityPulse();

// Drive the LED. Call from the main loop; requires cyw43_arch_init done.
void ledStatusTick(uint64_t nowMs);
