#pragma once

#include "flash_config.hpp"
#include "lcdtap/lcdtap.hpp"
#include "output_interface.hpp"

using SwitchIfaceFn = void (*)(lcdtap::BusType);
using SaveConfigFn = void (*)(const ConfigFile&);

// Initialize the USB CDC interface. Call once after stdio_init_all().
void uartIfInit(lcdtap::LcdTap* lcdtap, lcdtap::BusType* currentIface,
                OutputInterface* currentOutIf, SwitchIfaceFn switchIface,
                SaveConfigFn saveConfig);

// Non-blocking poll: receive characters, advance lexer/parser, and flush
// pending response output. Call from the main loop on Core 0.
void uartIfProcess();

// True once a client has changed the output interface. Changing it alters
// clk_sys, so the host must reboot rather than reconfigure in place.
bool uartIfRebootPending();

// True when no response is queued, i.e. it is safe to reboot without cutting
// off the acknowledgement the client is waiting for.
bool uartIfRespIdle();
