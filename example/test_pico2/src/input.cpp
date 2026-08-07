#include "testrig/input.hpp"

#include "hardware/gpio.h"
#include "pico/time.h"
#include "testrig/config.h"

namespace testrig {

namespace {

constexpr int DEBOUNCE_SAMPLES = 3;
constexpr uint16_t REPEAT_DELAY_MS = 500;   // hold time before repeating
constexpr uint16_t REPEAT_PERIOD_MS = 100;  // repeat interval

// Key bit positions in the debounced field (1 = pressed).
enum : uint8_t {
  BIT_DEC = 1u << 0,
  BIT_INC = 1u << 1,
  BIT_START = 1u << 2,
  BIT_SELECT = 1u << 3,
  BIT_BACK = 1u << 4,
};

struct Debounced {
  uint8_t raw = 0;       // last raw sample
  uint8_t count = 0;     // consecutive identical raw samples
  uint8_t accepted = 0;  // debounced value
  bool primed = false;   // accepted holds a real value
};

Debounced gKeys;
uint16_t gDecHoldMs = 0;
uint16_t gIncHoldMs = 0;

// Single-producer (timer IRQ) / single-consumer (main loop) ring.
constexpr int QUEUE_LEN = 16;
volatile InputEvent gQueue[QUEUE_LEN];
volatile uint32_t gQueueHead = 0;
volatile uint32_t gQueueTail = 0;

repeating_timer_t gTimer;

void push(InputEvent ev) {
  uint32_t next = (gQueueHead + 1) % QUEUE_LEN;
  if (next == gQueueTail) return;  // full: drop
  gQueue[gQueueHead] = ev;
  gQueueHead = next;
}

// Feed one raw sample; returns true when the accepted value changed.
bool debounce(Debounced* d, uint8_t raw) {
  if (raw != d->raw) {
    d->raw = raw;
    d->count = 1;
    return false;
  }
  if (d->count < DEBOUNCE_SAMPLES && ++d->count == DEBOUNCE_SAMPLES) {
    if (!d->primed) {
      d->primed = true;
      d->accepted = raw;
      return false;
    }
    if (raw != d->accepted) {
      // Caller reads the transition from accepted -> raw.
      return true;
    }
  }
  return false;
}

// Auto-repeat for a held key: first repeat after REPEAT_DELAY_MS, then one
// every REPEAT_PERIOD_MS. The initial press event comes from the edge.
void repeatTick(bool held, uint16_t* holdMs, InputEvent ev) {
  if (!held) {
    *holdMs = 0;
    return;
  }
  (*holdMs)++;
  if (*holdMs >= REPEAT_DELAY_MS) {
    push(ev);
    *holdMs = static_cast<uint16_t>(REPEAT_DELAY_MS - REPEAT_PERIOD_MS);
  }
}

bool timerCallback(repeating_timer_t*) {
  uint8_t keys =
      static_cast<uint8_t>((gpio_get(PIN_KEY_DEC) ? 0u : BIT_DEC) |
                           (gpio_get(PIN_KEY_INC) ? 0u : BIT_INC) |
                           (gpio_get(PIN_KEY_START) ? 0u : BIT_START) |
                           (gpio_get(PIN_KEY_SELECT) ? 0u : BIT_SELECT) |
                           (gpio_get(PIN_KEY_BACK) ? 0u : BIT_BACK));

  if (debounce(&gKeys, keys)) {
    uint8_t pressed = static_cast<uint8_t>(keys & ~gKeys.accepted);
    gKeys.accepted = keys;
    if (pressed & BIT_DEC) push(InputEvent::KEY_DEC);
    if (pressed & BIT_INC) push(InputEvent::KEY_INC);
    if (pressed & BIT_START) push(InputEvent::KEY_START);
    if (pressed & BIT_SELECT) push(InputEvent::KEY_SELECT);
    if (pressed & BIT_BACK) push(InputEvent::KEY_BACK);
  }

  repeatTick((gKeys.accepted & BIT_DEC) != 0, &gDecHoldMs, InputEvent::KEY_DEC);
  repeatTick((gKeys.accepted & BIT_INC) != 0, &gIncHoldMs, InputEvent::KEY_INC);
  return true;
}

}  // namespace

void inputInit() {
  const uint keyPins[] = {PIN_KEY_DEC, PIN_KEY_INC, PIN_KEY_START,
                          PIN_KEY_SELECT, PIN_KEY_BACK};
  for (uint pin : keyPins) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);  // all keys are low-active
  }

  add_repeating_timer_us(-1000, timerCallback, nullptr, &gTimer);
}

InputEvent inputPoll() {
  if (gQueueTail == gQueueHead) return InputEvent::NONE;
  InputEvent ev = gQueue[gQueueTail];
  gQueueTail = (gQueueTail + 1) % QUEUE_LEN;
  return ev;
}

void inputFlush() { gQueueTail = gQueueHead; }

}  // namespace testrig
