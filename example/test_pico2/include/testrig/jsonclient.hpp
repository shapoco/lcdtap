#pragma once

// JSON protocol client for the LcdTap target (UART_PROTOCOL.md).
//
// Small responses (getparams, getstats, ...) are buffered whole and parsed
// with ArduinoJson. getframebuffer responses can reach hundreds of KB, so
// their Base64 payload is streamed character by character into a
// FrameVerifier and never buffered.

#include <ArduinoJson.h>

#include <cstdint>

#include "testrig/link.hpp"
#include "testrig/verify.hpp"

namespace testrig {

class JsonClient {
 public:
  explicit JsonClient(Transport* t) : t_(t) {}

  // Switch between transports (CDC / WiFi HTTP) at runtime.
  void setTransport(Transport* t) { t_ = t; }
  Transport* transport() { return t_; }
  bool connected() { return t_ != nullptr && t_->connected(); }

  // hello round trip; true when the target answered "welcome lcdtap".
  bool hello(uint32_t timeoutMs = 1000);

  // Send a complete JSON command (without CRLF) and parse the one-line
  // response into doc. Skips non-JSON noise lines.
  bool command(const char* cmdLine, JsonDocument& doc, uint32_t timeoutMs);

  // getframebuffer with streaming compare. v must already be begin()'d; the
  // response header's width/height are checked against the verifier's
  // expectation. Returns false on transport/protocol errors or a dimension
  // mismatch (pixel mismatches are reported by the verifier itself).
  bool getFramebuffer(FrameVerifier& v, bool writeProtected,
                      uint32_t timeoutMs);

  // gettextbuffer: fills cols/rows and up to cap raw character codes.
  bool getTextBuffer(uint16_t* cols, uint16_t* rows, uint8_t* out, size_t cap,
                     uint32_t timeoutMs);

 private:
  bool send(const char* cmdLine, uint32_t timeoutMs);
  // Read one line into lineBuf_ (CR stripped); -1 on timeout.
  int readLine(uint32_t timeoutMs);
  // Consume exactly the literal s from the stream.
  bool expect(const char* s, uint32_t timeoutMs);
  // Read an unsigned integer, stopping at (and consuming) the delimiter.
  bool readUint(uint32_t* value, char delim, uint32_t timeoutMs);

  Transport* t_;
  static constexpr size_t LINE_CAP = 12288;
  char lineBuf_[LINE_CAP];
};

}  // namespace testrig
