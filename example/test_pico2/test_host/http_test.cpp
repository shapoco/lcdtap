// Host-side unit test for the streaming HTTP response parser
// (../src/http_client.cpp, no MCU dependencies).
//
// Covers the pico2w_remote server's framing (chunked + Connection: close),
// Content-Length and read-to-close bodies, split delivery down to
// byte-by-byte feeds, 503 status reporting, and malformed input.
//
// Build & run:
//   g++ -O2 -Wall -Wextra -I../include -o /tmp/testrig_http_test
//       http_test.cpp ../src/http_client.cpp
//   /tmp/testrig_http_test

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "testrig/http_client.hpp"

using namespace testrig;

static int gFailures = 0;

#define CHECK(cond, ...)                            \
  do {                                              \
    if (!(cond)) {                                  \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                          \
      printf("\n");                                 \
      gFailures++;                                  \
    }                                               \
  } while (0)

static std::string gOut;
static void sink(void*, uint8_t b) { gOut.push_back(static_cast<char>(b)); }

// Feed the whole response in chunks of feedSize bytes.
static bool run(HttpResponseParser& p, const std::string& raw,
                size_t feedSize) {
  gOut.clear();
  p.begin(sink, nullptr);
  for (size_t i = 0; i < raw.size(); i += feedSize) {
    size_t n = raw.size() - i;
    if (n > feedSize) n = feedSize;
    if (!p.feed(reinterpret_cast<const uint8_t*>(raw.data()) + i, n)) {
      return false;
    }
  }
  return true;
}

static const char kChunked[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Transfer-Encoding: chunked\r\n"
    "Connection: close\r\n"
    "\r\n"
    "b\r\n"
    "{\"response\"\r\n"
    "10\r\n"
    ":\"ok\"}\r\nextrapad\r\n"
    "0\r\n"
    "\r\n";
static const char kChunkedBody[] = "{\"response\":\"ok\"}\r\nextrapad";

static void testChunked() {
  printf("testChunked\n");
  for (size_t feed : {size_t(1), size_t(3), size_t(7), strlen(kChunked)}) {
    HttpResponseParser p;
    CHECK(run(p, kChunked, feed), "feed=%zu parse error", feed);
    CHECK(p.statusCode() == 200, "feed=%zu status %d", feed, p.statusCode());
    CHECK(p.bodyComplete(), "feed=%zu body incomplete", feed);
    CHECK(gOut == kChunkedBody, "feed=%zu body '%s'", feed, gOut.c_str());
    CHECK(p.finishOnClose(), "feed=%zu finishOnClose", feed);
  }
}

static void testContentLength() {
  printf("testContentLength\n");
  std::string raw =
      "HTTP/1.1 200 OK\r\n"
      "content-length: 5\r\n"
      "\r\n"
      "hello";
  for (size_t feed : {size_t(1), size_t(4), raw.size()}) {
    HttpResponseParser p;
    CHECK(run(p, raw, feed), "feed=%zu parse error", feed);
    CHECK(p.bodyComplete(), "feed=%zu incomplete", feed);
    CHECK(gOut == "hello", "feed=%zu body '%s'", feed, gOut.c_str());
  }
}

static void testReadToClose() {
  printf("testReadToClose\n");
  std::string raw =
      "HTTP/1.1 200 OK\r\n"
      "Connection: close\r\n"
      "\r\n"
      "streamed until close";
  HttpResponseParser p;
  CHECK(run(p, raw, 5), "parse error");
  CHECK(!p.bodyComplete(), "complete before close");
  CHECK(p.finishOnClose(), "finishOnClose");
  CHECK(p.bodyComplete(), "complete after close");
  CHECK(gOut == "streamed until close", "body '%s'", gOut.c_str());
}

static void testBusy503() {
  printf("testBusy503\n");
  std::string raw =
      "HTTP/1.1 503 Service Unavailable\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  HttpResponseParser p;
  CHECK(run(p, raw, 2), "parse error");
  CHECK(p.statusCode() == 503, "status %d", p.statusCode());
  CHECK(p.bodyComplete(), "zero-length body must complete");
  CHECK(gOut.empty(), "body not empty");
}

static void testChunkExtensionAndCase() {
  printf("testChunkExtensionAndCase\n");
  std::string raw =
      "HTTP/1.1 200 OK\r\n"
      "TRANSFER-ENCODING: Chunked\r\n"
      "\r\n"
      "5;ext=1\r\n"
      "abcde\r\n"
      "0\r\n"
      "\r\n";
  HttpResponseParser p;
  CHECK(run(p, raw, 1), "parse error");
  CHECK(p.bodyComplete(), "incomplete");
  CHECK(gOut == "abcde", "body '%s'", gOut.c_str());
}

static void testMalformed() {
  printf("testMalformed\n");
  {
    HttpResponseParser p;
    CHECK(!run(p, "GARBAGE\r\n\r\n", 4), "no-space status must fail");
    CHECK(p.error(), "error flag");
  }
  {
    HttpResponseParser p;
    // Chunk size line without hex digits.
    std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "zz\r\n";
    CHECK(!run(p, raw, raw.size()), "bad chunk size must fail");
  }
  {
    HttpResponseParser p;
    // Truncated chunked body: close before the terminator.
    std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nab";
    CHECK(run(p, raw, raw.size()), "prefix must parse");
    CHECK(!p.finishOnClose(), "truncated body must not complete on close");
  }
}

int main() {
  testChunked();
  testContentLength();
  testReadToClose();
  testBusy503();
  testChunkExtensionAndCase();
  testMalformed();
  if (gFailures == 0) {
    printf("ALL TESTS PASSED\n");
    return 0;
  }
  printf("%d FAILURE(S)\n", gFailures);
  return 1;
}
