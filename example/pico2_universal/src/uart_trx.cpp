#include "uart_trx.hpp"

#include "pico/stdio.h"
#include "tusb.h"

void trxInit() { stdio_init_all(); }

bool trxIsConnected() { return tud_cdc_connected(); }

int trxGetChar() { return getchar_timeout_us(0); }

int trxWrite(const char* data, int len) {
  if (!tud_cdc_connected()) return 0;
  int avail = static_cast<int>(tud_cdc_write_available());
  if (avail == 0) return 0;
  if (len > avail) len = avail;
  int written =
      static_cast<int>(tud_cdc_write(data, static_cast<uint32_t>(len)));
  if (written > 0) tud_cdc_write_flush();
  return written;
}

void trxFlush() { tud_cdc_write_flush(); }

int trxWriteAvail() { return static_cast<int>(tud_cdc_write_available()); }
