#pragma once

#include "flash_config.hpp"
#include "lcdtap/lcdtap.hpp"

using SwitchIfaceFn = void (*)(lcdtap::BusType);
using SaveConfigFn = void (*)(const ConfigFile&);

// Initialize the USB CDC interface. Call once after stdio_init_all().
void uartIfInit(lcdtap::LcdTap* lcdtap, lcdtap::BusType* currentIface,
                SwitchIfaceFn switchIface, SaveConfigFn saveConfig);

// Non-blocking poll: receive characters, advance lexer/parser, and flush
// pending response output. Call from the main loop on Core 0.
void uartIfProcess();
