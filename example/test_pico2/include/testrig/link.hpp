#pragma once

// Transport abstraction for the target's JSON protocol.
//
// The pico2_universal target speaks it over USB CDC; pico2w_remote speaks
// the same protocol over HTTP POST /api. Keeping the executor on this
// interface lets a future WiFi transport drop in without touching test
// logic.

#include <cstddef>
#include <cstdint>

namespace testrig {

class Transport {
 public:
  virtual ~Transport() = default;

  // Target link is up (e.g. CDC mounted).
  virtual bool connected() = 0;

  // Send one complete command line (caller includes the trailing CRLF).
  // Returns false on disconnect/timeout.
  virtual bool sendLine(const char* line, size_t len, uint32_t timeoutMs) = 0;

  // Receive one response character; -1 on timeout or disconnect.
  virtual int recvChar(uint32_t timeoutMs) = 0;

  // Drop any buffered unread response data (resync after an error).
  virtual void drainInput() = 0;
};

// WiFi HTTP transport singleton (link_http.cpp): POST /api per command
// against the pico2w_remote target. Set the target IP (from its CDC
// netstatus) before use.
Transport* httpTransport();
void httpTransportSetTarget(uint32_t ipv4NetOrder);

// USB CDC transport backed by usb_host.hpp rings.
class CdcTransport : public Transport {
 public:
  bool connected() override;
  bool sendLine(const char* line, size_t len, uint32_t timeoutMs) override;
  int recvChar(uint32_t timeoutMs) override;
  void drainInput() override;
};

}  // namespace testrig
