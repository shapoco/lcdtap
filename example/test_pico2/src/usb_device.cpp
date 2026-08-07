// PC-facing device-side glue. pico_stdio_usb disables itself when
// tinyusb_host is linked, so printf is routed to the device CDC through a
// minimal custom stdio driver instead. Best-effort: output is dropped when
// the PC is not connected or the FIFO is full (logging must never stall a
// test run).

#include "testrig/usb_device.hpp"
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "tusb.h"

namespace testrig {

namespace {

void stdioOutChars(const char* buf, int len) {
  if (!tud_cdc_connected()) return;
  tud_cdc_write(buf, static_cast<uint32_t>(len));
  tud_cdc_write_flush();
}

void stdioOutFlush() {
  if (tud_cdc_connected()) tud_cdc_write_flush();
}

int stdioInChars(char* buf, int len) {
  if (!tud_cdc_connected()) return PICO_ERROR_NO_DATA;
  uint32_t n = tud_cdc_read(buf, static_cast<uint32_t>(len));
  return n > 0 ? static_cast<int>(n) : PICO_ERROR_NO_DATA;
}

stdio_driver_t gStdioDriver = {
    .out_chars = stdioOutChars,
    .out_flush = stdioOutFlush,
    .in_chars = stdioInChars,
    .set_chars_available_callback = nullptr,
    .next = nullptr,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .last_ended_with_cr = false,
    .crlf_enabled = true,
#endif
};

}  // namespace

void usbDeviceStdioInit() { stdio_set_driver_enabled(&gStdioDriver, true); }

}  // namespace testrig
