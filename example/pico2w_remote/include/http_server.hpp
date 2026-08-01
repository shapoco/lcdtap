#pragma once

// Minimal HTTP server on raw lwIP TCP.
//
//   GET  /...   — embedded static assets (web_assets)
//   POST /api   — JSON command, request body fed to a dedicated JsonIntf
//                 instance, response streamed back with chunked encoding
//
// One API request is in flight at a time; concurrent POSTs get 503.

#include "lcdtap/pico2/json_intf.hpp"

// apiIntf must be initialized with the HTTP transport returned by
// httpServerTransport() before the first request arrives.
void httpServerInit(JsonIntf* apiIntf);

// Transport vtable feeding the current API connection.
JsonIntfTransport httpServerTransport();

// Pump connections; call from the main loop next to cyw43_arch_poll().
void httpServerProcess();

// True while an API connection is open (gates reboots so the response is
// not cut off).
bool httpServerApiBusy();
