#pragma once

// Non-blocking transport layer wrapping USB CDC (TinyUSB).
// Replace uart_trx.cpp to port to WiFi or Bluetooth.

void trxInit();
bool trxIsConnected();

// Returns received character, or -1 if none available.
int trxGetChar();

// Non-blocking write: caps at available TX buffer space, flushes after write.
// Returns bytes actually written (0 if buffer full or disconnected).
int trxWrite(const char* data, int len);

void trxFlush();
int trxWriteAvail();
