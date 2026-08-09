#pragma once

// Streaming HTTP/1.1 response parser (platform independent, host-testable).
//
// Feed raw bytes as they arrive from the socket; decoded body bytes are
// emitted through a sink callback. Supports Transfer-Encoding: chunked
// (what pico2w_remote sends), Content-Length, and read-to-close bodies.
// The pico2w_remote server closes the connection after every response
// (Connection: close), so one parser instance serves one transaction.

#include <cstddef>
#include <cstdint>

namespace testrig {

class HttpResponseParser {
 public:
  using Sink = void (*)(void* ctx, uint8_t b);

  void begin(Sink sink, void* ctx);

  // Feed raw connection bytes. Returns false on a protocol error (the
  // parser latches the error state).
  bool feed(const uint8_t* data, size_t n);

  // Call when the peer closed the connection. Returns true when the
  // response is complete (always true for read-to-close bodies).
  bool finishOnClose();

  // 0 until the status line has been parsed.
  int statusCode() const { return status_; }
  bool headersDone() const;
  bool bodyComplete() const;
  bool error() const;

 private:
  enum class St : uint8_t {
    STATUS_LINE,
    HEADER_LINE,
    CHUNK_SIZE,
    CHUNK_DATA,
    CHUNK_DATA_CR,
    CHUNK_DATA_LF,
    CHUNK_END,  // after the 0-size chunk: consume trailing CRLF
    BODY_CL,
    BODY_TO_CLOSE,
    DONE,
    ERROR,
  };

  void emit(uint8_t b) { sink_(sinkCtx_, b); }
  void headerLineDone();

  Sink sink_ = nullptr;
  void* sinkCtx_ = nullptr;
  St st_ = St::STATUS_LINE;
  int status_ = 0;
  bool chunked_ = false;
  bool haveCl_ = false;
  uint32_t contentLength_ = 0;
  uint32_t remaining_ = 0;  // bytes left in the current chunk / CL body
  char line_[128];
  size_t lineLen_ = 0;
};

}  // namespace testrig
