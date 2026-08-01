#pragma once

// USB CDC (TinyUSB) transport for the JSON command interface.
//
// Compiled into each executable via the pico2_json_cdc INTERFACE library so
// it picks up that target's stdio/TinyUSB configuration.

#include "lcdtap/pico2/json_intf.hpp"

// Calls stdio_init_all(). Call once before jsonIntfProcess().
void cdcTrxInit();

// Transport vtable bound to the USB CDC functions (ctx is unused).
JsonIntfTransport cdcTrxTransport();
