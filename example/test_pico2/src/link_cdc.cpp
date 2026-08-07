#include "pico/stdlib.h"
#include "testrig/link.hpp"
#include "testrig/usb_host.hpp"

namespace testrig {

bool CdcTransport::connected() { return usbHostCdcMounted(); }

bool CdcTransport::sendLine(const char* line, size_t len, uint32_t timeoutMs) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(line);
  size_t sent = 0;
  absolute_time_t deadline = make_timeout_time_ms(timeoutMs);
  while (sent < len) {
    if (!usbHostCdcMounted()) return false;
    sent += usbHostCdcWrite(p + sent, static_cast<uint32_t>(len - sent));
    if (sent < len) {
      if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
        return false;
      }
      sleep_us(100);
    }
  }
  return true;
}

int CdcTransport::recvChar(uint32_t timeoutMs) {
  uint8_t b;
  absolute_time_t deadline = make_timeout_time_ms(timeoutMs);
  while (usbHostCdcRead(&b, 1) == 0) {
    if (!usbHostCdcMounted()) return -1;
    if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) return -1;
    sleep_us(50);
  }
  return b;
}

void CdcTransport::drainInput() { usbHostCdcDrainInput(); }

}  // namespace testrig
