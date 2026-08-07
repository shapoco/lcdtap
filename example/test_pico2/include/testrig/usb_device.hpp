#pragma once

// PC-facing device side (native USB, core 0): CDC debug console stdio
// driver; Phase 2 adds the MSC passthrough interface here.

namespace testrig {

// Route printf to the device CDC (call after tud_init).
void usbDeviceStdioInit();

}  // namespace testrig
