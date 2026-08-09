#pragma once

// UI state machine: vector select -> item select -> value select,
// test running / result screens, and the download-mode overlay.

#include "testrig/jsonclient.hpp"

namespace testrig {

// cdcTransport is the idle/default transport; the WiFi HTTP transport is
// selected per run from the title screen setting.
void uiInit(JsonClient* client, Transport* cdcTransport);

// Pump from the main loop: handles input, MSC overlay, blink and redraw.
// Test execution runs synchronously inside (Start key).
void uiTick();

}  // namespace testrig
