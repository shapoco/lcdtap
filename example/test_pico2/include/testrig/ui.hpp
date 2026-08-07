#pragma once

// UI state machine: vector select -> item select -> value select,
// test running / result screens, and the download-mode overlay.

#include "testrig/jsonclient.hpp"

namespace testrig {

void uiInit(JsonClient* client);

// Pump from the main loop: handles input, MSC overlay, blink and redraw.
// Test execution runs synchronously inside (Start key).
void uiTick();

}  // namespace testrig
