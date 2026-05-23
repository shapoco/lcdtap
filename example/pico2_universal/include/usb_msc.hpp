#pragma once

#include "lcdtap/lcdtap.hpp"

// Run USB mass storage mode. Never returns.
// Calls process_input_cb() in the main loop until the first data-sector read
// freezes the framebuffer snapshot. Enter key triggers a watchdog reset.
[[noreturn]] void runUsbMassStorage(lcdtap::LcdTap *inst,
                                    void (*process_input_cb)());
