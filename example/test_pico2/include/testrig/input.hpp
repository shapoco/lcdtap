#pragma once

// Key input with timer-driven debouncing and auto-repeat.
//
// Five tact switches, all low-active with internal pull-ups, sampled at
// 1 ms; a level is accepted after 3 identical consecutive samples. The
// Dec/Inc keys auto-repeat while held (500 ms initial delay, then every
// 100 ms).

#include <cstdint>

namespace testrig {

enum class InputEvent : uint8_t {
  NONE,
  KEY_DEC,     // selection decrement (GPIO2, auto-repeats)
  KEY_INC,     // selection increment (GPIO3, auto-repeats)
  KEY_START,   // Start pressed
  KEY_SELECT,  // Select pressed
  KEY_BACK,    // Back pressed
};

void inputInit();

// Dequeue the next event (NONE when the queue is empty).
InputEvent inputPoll();

// Drop any queued events (e.g. when leaving a modal screen).
void inputFlush();

}  // namespace testrig
