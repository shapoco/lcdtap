#include "lcdtap/pico2/cdc_trx.hpp"

#include "pico/stdio.h"
#include "tusb.h"

static bool cdcIsConnected(void* /*ctx*/) { return tud_cdc_connected(); }

static int cdcGetChar(void* /*ctx*/) { return getchar_timeout_us(0); }

// Non-blocking write: caps at available TX buffer space, flushes after write.
// Returns bytes actually written (0 if buffer full or disconnected).
static int cdcWrite(void* /*ctx*/, const char* data, int len) {
  if (!tud_cdc_connected()) return 0;
  int avail = static_cast<int>(tud_cdc_write_available());
  if (avail == 0) return 0;
  if (len > avail) len = avail;
  int written =
      static_cast<int>(tud_cdc_write(data, static_cast<uint32_t>(len)));
  if (written > 0) tud_cdc_write_flush();
  return written;
}

void cdcTrxInit() { stdio_init_all(); }

JsonIntfTransport cdcTrxTransport() {
  return {nullptr, cdcIsConnected, cdcGetChar, cdcWrite};
}
