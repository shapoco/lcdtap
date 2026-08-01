#include "led_status.hpp"

#include "pico/cyw43_arch.h"

static LedPattern gPattern = LedPattern::UNCONFIGURED;
static uint64_t gPatternStartMs = 0;
static bool gPulseReq = false;
static uint64_t gPulseOffUntilMs = 0;
static uint64_t gLastPulseStartMs = 0;
static bool gCurrentLevel = false;
static bool gLevelKnown = false;

static constexpr uint32_t PULSE_OFF_MS = 50u;
static constexpr uint32_t PULSE_MIN_PERIOD_MS = 100u;

void ledStatusSetPattern(LedPattern p) {
  if (p == gPattern) return;
  gPattern = p;
  gPatternStartMs = 0;  // realign the blink phase on the next tick
  gPulseReq = false;
  gPulseOffUntilMs = 0;
}

void ledActivityPulse() {
  if (gPattern == LedPattern::CONNECTED) gPulseReq = true;
}

void ledStatusTick(uint64_t nowMs) {
  bool on = true;
  switch (gPattern) {
    case LedPattern::UNCONFIGURED:
    case LedPattern::CONNECTING:
    case LedPattern::FAILED: {
      uint32_t onMs, offMs;
      if (gPattern == LedPattern::UNCONFIGURED) {
        onMs = 250u;
        offMs = 750u;
      } else if (gPattern == LedPattern::CONNECTING) {
        onMs = 250u;
        offMs = 250u;
      } else {
        onMs = 500u;
        offMs = 500u;
      }
      if (gPatternStartMs == 0) gPatternStartMs = nowMs;
      uint64_t phase = (nowMs - gPatternStartMs) % (onMs + offMs);
      on = phase < onMs;
      break;
    }
    case LedPattern::CONNECTED: {
      // Steady ON; an activity event inserts a short OFF pulse, with a
      // minimum period between pulse starts so heavy traffic still reads as
      // flicker instead of a dark LED.
      if (gPulseReq) {
        gPulseReq = false;
        if (gLastPulseStartMs == 0 ||
            nowMs - gLastPulseStartMs >= PULSE_MIN_PERIOD_MS) {
          gPulseOffUntilMs = nowMs + PULSE_OFF_MS;
          gLastPulseStartMs = nowMs;
        }
      }
      if (nowMs >= gPulseOffUntilMs) gPulseOffUntilMs = 0;
      on = (gPulseOffUntilMs == 0);
      break;
    }
  }

  // cyw43_arch_gpio_put goes over the CYW43 bus; only write on change.
  if (!gLevelKnown || on != gCurrentLevel) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
    gCurrentLevel = on;
    gLevelKnown = true;
  }
}
