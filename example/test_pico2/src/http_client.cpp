#include "testrig/http_client.hpp"

#include <cstring>

namespace testrig {

namespace {

bool asciiCaseEq(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb) return false;
  }
  return true;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

void HttpResponseParser::begin(Sink sink, void* ctx) {
  sink_ = sink;
  sinkCtx_ = ctx;
  st_ = St::STATUS_LINE;
  status_ = 0;
  chunked_ = false;
  haveCl_ = false;
  contentLength_ = 0;
  remaining_ = 0;
  lineLen_ = 0;
}

bool HttpResponseParser::headersDone() const {
  return st_ != St::STATUS_LINE && st_ != St::HEADER_LINE && st_ != St::ERROR;
}

bool HttpResponseParser::bodyComplete() const { return st_ == St::DONE; }

bool HttpResponseParser::error() const { return st_ == St::ERROR; }

// End of one header line in line_: dispatch on the interesting headers, or
// on the empty line select the body framing.
void HttpResponseParser::headerLineDone() {
  line_[lineLen_] = '\0';
  if (lineLen_ == 0) {
    // Blank line: headers finished.
    if (chunked_) {
      st_ = St::CHUNK_SIZE;
    } else if (haveCl_) {
      remaining_ = contentLength_;
      st_ = (remaining_ == 0) ? St::DONE : St::BODY_CL;
    } else {
      st_ = St::BODY_TO_CLOSE;
    }
    lineLen_ = 0;
    return;
  }
  static const char TE[] = "transfer-encoding:";
  static const char CL[] = "content-length:";
  if (lineLen_ > sizeof(TE) - 1 && asciiCaseEq(line_, TE, sizeof(TE) - 1)) {
    if (strstr(line_, "chunked") != nullptr ||
        strstr(line_, "Chunked") != nullptr) {
      chunked_ = true;
    }
  } else if (lineLen_ > sizeof(CL) - 1 &&
             asciiCaseEq(line_, CL, sizeof(CL) - 1)) {
    uint32_t v = 0;
    for (const char* p = line_ + sizeof(CL) - 1; *p != '\0'; p++) {
      if (*p == ' ') continue;
      if (*p < '0' || *p > '9') break;
      v = v * 10u + static_cast<uint32_t>(*p - '0');
    }
    contentLength_ = v;
    haveCl_ = true;
  }
  lineLen_ = 0;
}

bool HttpResponseParser::feed(const uint8_t* data, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t b = data[i];
    switch (st_) {
      case St::STATUS_LINE:
        if (b == '\n') {
          line_[lineLen_] = '\0';
          // "HTTP/1.x SP status ..."
          const char* sp = strchr(line_, ' ');
          if (sp == nullptr) {
            st_ = St::ERROR;
            return false;
          }
          status_ = 0;
          for (const char* p = sp + 1; *p >= '0' && *p <= '9'; p++) {
            status_ = status_ * 10 + (*p - '0');
          }
          if (status_ < 100 || status_ > 599) {
            st_ = St::ERROR;
            return false;
          }
          lineLen_ = 0;
          st_ = St::HEADER_LINE;
        } else if (b != '\r' && lineLen_ + 1 < sizeof(line_)) {
          line_[lineLen_++] = static_cast<char>(b);
        }
        break;

      case St::HEADER_LINE:
        if (b == '\n') {
          headerLineDone();
          if (st_ == St::ERROR) return false;
        } else if (b != '\r' && lineLen_ + 1 < sizeof(line_)) {
          line_[lineLen_++] = static_cast<char>(b);
        }
        break;

      case St::CHUNK_SIZE:
        if (b == '\n') {
          line_[lineLen_] = '\0';
          uint32_t size = 0;
          bool any = false;
          for (const char* p = line_; *p != '\0' && *p != ';'; p++) {
            int v = hexVal(*p);
            if (v < 0) break;
            size = (size << 4) | static_cast<uint32_t>(v);
            any = true;
          }
          lineLen_ = 0;
          if (!any) {
            st_ = St::ERROR;
            return false;
          }
          if (size == 0) {
            st_ = St::CHUNK_END;
          } else {
            remaining_ = size;
            st_ = St::CHUNK_DATA;
          }
        } else if (b != '\r' && lineLen_ + 1 < sizeof(line_)) {
          line_[lineLen_++] = static_cast<char>(b);
        }
        break;

      case St::CHUNK_DATA:
        emit(b);
        if (--remaining_ == 0) st_ = St::CHUNK_DATA_CR;
        break;

      case St::CHUNK_DATA_CR:
        // CRLF after the chunk payload (tolerate a bare LF).
        if (b == '\r') {
          st_ = St::CHUNK_DATA_LF;
        } else if (b == '\n') {
          st_ = St::CHUNK_SIZE;
        } else {
          st_ = St::ERROR;
          return false;
        }
        break;

      case St::CHUNK_DATA_LF:
        if (b != '\n') {
          st_ = St::ERROR;
          return false;
        }
        st_ = St::CHUNK_SIZE;
        break;

      case St::CHUNK_END:
        // Consume the CRLF (and any trailers) after the 0-size chunk; the
        // first LF completes the response.
        if (b == '\n') st_ = St::DONE;
        break;

      case St::BODY_CL:
        emit(b);
        if (--remaining_ == 0) st_ = St::DONE;
        break;

      case St::BODY_TO_CLOSE: emit(b); break;

      case St::DONE: break;  // ignore trailing bytes

      case St::ERROR: return false;
    }
  }
  return st_ != St::ERROR;
}

bool HttpResponseParser::finishOnClose() {
  if (st_ == St::BODY_TO_CLOSE) {
    st_ = St::DONE;
    return true;
  }
  return st_ == St::DONE;
}

}  // namespace testrig
