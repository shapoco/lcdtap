#include <cstdio>
#include <cstring>

#include "lwip/ip_addr.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "testrig/http_client.hpp"
#include "testrig/link.hpp"
#include "testrig/wifi_mgr.hpp"

// WiFi HTTP transport for the JSON protocol: one POST /api per command
// against the pico2w_remote target (which answers chunked + Connection:
// close). All of this runs on core 0 under pico_cyw43_arch_lwip_poll, so
// the lwIP callbacks and the Transport calls never race.

namespace testrig {

namespace {

constexpr uint16_t HTTP_PORT = 80;
constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;
constexpr int MAX_503_RETRY = 3;

ip_addr_t gTargetIp;
bool gTargetSet = false;

// Decoded response ring with real TCP backpressure: tcp_recved is DEFERRED
// until the consumer has drained the ring below half. The server can then
// never have more than TCP_WND (11.7 KB) of un-acknowledged raw bytes
// outstanding, so the ring fill is bounded by RING_CAP/2 + TCP_WND. (With
// immediate recved, one cyw43_arch_poll() can loop ACK -> more data -> ACK
// indefinitely on a fast LAN and overflow any fixed ring — observed on
// hardware as random-offset ring-overflow failures.)
constexpr size_t RING_CAP = 32 * 1024;  // power-of-two not required
uint8_t gRing[RING_CAP];
volatile size_t gRingHead = 0;
volatile size_t gRingTail = 0;

struct tcp_pcb* gPcb = nullptr;
HttpResponseParser gParser;
bool gConnected = false;   // TCP connect completed
bool gClosed = false;      // peer closed / aborted
bool gError = false;       // transport-level failure for this transaction
bool gTerminated = false;  // '\n' terminator pushed

char gReqLine[1536];
size_t gReqLen = 0;

// Failure diagnostics: printed on the PC console so intermittent WiFi
// failures can be attributed to a concrete site.
const char* gLastFail = "";
uint32_t gRxRawBytes = 0;  // raw TCP bytes of the current transaction
uint32_t gUnrecved = 0;    // raw bytes not yet tcp_recved (window hold-back)

void ringPush(uint8_t b) {
  size_t next = (gRingHead + 1) % RING_CAP;
  if (next == gRingTail) {
    if (!gError) gLastFail = "ring-overflow";
    gError = true;  // overflow: transaction is already lost
    return;
  }
  gRing[gRingHead] = b;
  gRingHead = next;
}

int ringPop() {
  if (gRingTail == gRingHead) return -1;
  uint8_t b = gRing[gRingTail];
  gRingTail = (gRingTail + 1) % RING_CAP;
  return b;
}

size_t ringFill() { return (gRingHead + RING_CAP - gRingTail) % RING_CAP; }

// Re-open the TCP receive window once the consumer has drained enough.
void maybeRecved() {
  if (gPcb != nullptr && gUnrecved != 0 && ringFill() < RING_CAP / 2) {
    uint32_t n = gUnrecved;
    gUnrecved = 0;
    tcp_recved(gPcb, static_cast<u16_t>(n > 0xFFFFu ? 0xFFFFu : n));
  }
}

void ringReset() { gRingHead = gRingTail = 0; }

void parserSink(void* /*ctx*/, uint8_t b) { ringPush(b); }

void closePcb(bool abort) {
  if (gPcb == nullptr) return;
  tcp_arg(gPcb, nullptr);
  tcp_recv(gPcb, nullptr);
  tcp_err(gPcb, nullptr);
  tcp_sent(gPcb, nullptr);
  if (abort) {
    tcp_abort(gPcb);
  } else if (tcp_close(gPcb) != ERR_OK) {
    tcp_abort(gPcb);
  }
  gPcb = nullptr;
}

// Response fully received: terminate the line for JsonClient.
void maybeTerminate() {
  if (!gTerminated && !gError && gParser.bodyComplete()) {
    gTerminated = true;
    ringPush('\n');
  }
}

err_t onRecv(void* /*arg*/, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
  if (p == nullptr) {
    // Peer closed (normal for Connection: close).
    gClosed = true;
    if (gParser.finishOnClose()) {
      maybeTerminate();
    } else if (!gParser.bodyComplete()) {
      if (!gError) gLastFail = "closed-incomplete";
      gError = true;
    }
    closePcb(false);
    return ERR_OK;
  }
  if (err == ERR_OK) {
    gRxRawBytes += p->tot_len;
    for (struct pbuf* q = p; q != nullptr; q = q->next) {
      if (!gParser.feed(static_cast<const uint8_t*>(q->payload), q->len)) {
        if (!gError) gLastFail = "parser-error";
        gError = true;
        break;
      }
    }
    maybeTerminate();
    gUnrecved += p->tot_len;
  }
  pbuf_free(p);
  return ERR_OK;
}

void onErr(void* /*arg*/, err_t err) {
  // pcb is already freed by lwIP.
  gPcb = nullptr;
  gClosed = true;
  if (!gParser.bodyComplete()) {
    if (!gError) {
      static char buf[24];
      snprintf(buf, sizeof(buf), "tcp-err %d", (int)err);
      gLastFail = buf;
    }
    gError = true;
  }
}

err_t onConnected(void* /*arg*/, struct tcp_pcb* /*pcb*/, err_t err) {
  if (err != ERR_OK) {
    gError = true;
    return ERR_OK;
  }
  gConnected = true;
  return ERR_OK;
}

// Run one POST transaction with the buffered request line. Returns false on
// connect/send failure; response status handling is the caller's business.
bool startRequest(uint32_t timeoutMs) {
  gLastFail = "";
  gRxRawBytes = 0;
  gUnrecved = 0;
  ringReset();
  gParser.begin(parserSink, nullptr);
  gConnected = false;
  gClosed = false;
  gError = false;
  gTerminated = false;

  gPcb = tcp_new_ip_type(IPADDR_TYPE_V4);
  if (gPcb == nullptr) {
    gLastFail = "no-pcb";
    return false;
  }
  tcp_recv(gPcb, onRecv);
  tcp_err(gPcb, onErr);
  if (tcp_connect(gPcb, &gTargetIp, HTTP_PORT, onConnected) != ERR_OK) {
    gLastFail = "connect-call";
    closePcb(true);
    return false;
  }

  absolute_time_t deadline = make_timeout_time_ms(CONNECT_TIMEOUT_MS);
  while (!gConnected && !gError && gPcb != nullptr) {
    cyw43_arch_poll();
    if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
      gLastFail = "connect-timeout";
      closePcb(true);
      return false;
    }
    sleep_us(100);
  }
  if (gError || gPcb == nullptr) {
    if (gLastFail[0] == '\0') gLastFail = "connect-refused";
    return false;
  }

  char header[128];
  int hn = snprintf(header, sizeof(header),
                    "POST /api HTTP/1.1\r\n"
                    "Host: target\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %u\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    static_cast<unsigned>(gReqLen));
  if (tcp_write(gPcb, header, static_cast<u16_t>(hn), TCP_WRITE_FLAG_COPY) !=
          ERR_OK ||
      tcp_write(gPcb, gReqLine, static_cast<u16_t>(gReqLen),
                TCP_WRITE_FLAG_COPY) != ERR_OK) {
    gLastFail = "send";
    closePcb(true);
    return false;
  }
  tcp_output(gPcb);

  // Wait until the status line is known (or the transaction dies) so the
  // caller can react to 503.
  deadline = make_timeout_time_ms(timeoutMs);
  while (gParser.statusCode() == 0 && !gError) {
    if (gClosed) break;
    maybeRecved();
    cyw43_arch_poll();
    if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
      gLastFail = "status-timeout";
      closePcb(true);
      return false;
    }
    sleep_us(100);
  }
  return !gError && gParser.statusCode() != 0;
}

void abortTransaction() {
  closePcb(true);
  ringReset();
  gError = false;
  gTerminated = false;
}

}  // namespace

void httpTransportSetTarget(uint32_t ipv4NetOrder) {
  ip_addr_set_ip4_u32(&gTargetIp, ipv4NetOrder);
  gTargetSet = true;
}

class HttpTransport : public Transport {
 public:
  bool connected() override {
    return gTargetSet && wifiMgrState() == WifiState::CONNECTED;
  }

  bool sendLine(const char* line, size_t len, uint32_t timeoutMs) override {
    if (!connected()) return false;
    // JsonClient sends the command body and the trailing CRLF as separate
    // calls; buffer until the newline, then run the HTTP transaction.
    for (size_t i = 0; i < len; i++) {
      char c = line[i];
      if (c == '\n') {
        bool ok = false;
        for (int attempt = 0; attempt < MAX_503_RETRY; attempt++) {
          if (!startRequest(timeoutMs)) {
            // Connection-level hiccup (connect/status timeout): a fresh
            // connection usually succeeds, and all protocol commands are
            // idempotent, so retrying is safe.
            abortTransaction();
            sleep_ms(100);
            continue;
          }
          if (gParser.statusCode() == 503) {
            // Target busy (one API request at a time): back off briefly.
            abortTransaction();
            absolute_time_t until = make_timeout_time_ms(200);
            while (absolute_time_diff_us(get_absolute_time(), until) > 0) {
              cyw43_arch_poll();
              sleep_us(100);
            }
            continue;
          }
          ok = (gParser.statusCode() == 200);
          if (!ok) {
            gLastFail = "non-200";
            abortTransaction();
          }
          break;
        }
        gReqLen = 0;
        if (!ok) {
          gError = true;
          printf("http: request failed (%s, status=%d)\n", gLastFail,
                 gParser.statusCode());
        }
        return ok;
      }
      if (c != '\r' && gReqLen + 1 < sizeof(gReqLine)) {
        gReqLine[gReqLen++] = c;
      }
    }
    return true;
  }

  int recvChar(uint32_t timeoutMs) override {
    absolute_time_t deadline = make_timeout_time_ms(timeoutMs);
    while (true) {
      int b = ringPop();
      if (b >= 0) {
        maybeRecved();
        return b;
      }
      if (gError) {
        reportOnce("recv-error");
        return -1;
      }
      if (gClosed && gPcb == nullptr && gTerminated) {
        // Everything delivered; nothing more will come.
        return -1;
      }
      if (!connected()) {
        reportOnce("wifi-down");
        return -1;
      }
      maybeRecved();
      cyw43_arch_poll();
      if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
        gLastFail = "recv-timeout";
        reportOnce("recv-timeout");
        return -1;
      }
      sleep_us(100);
    }
  }

  void drainInput() override {
    abortTransaction();
    gReqLen = 0;
    gReported = false;
  }

 private:
  void reportOnce(const char* where) {
    if (gReported) return;
    gReported = true;
    printf("http: %s (%s) raw=%lu ring=%u closed=%d term=%d status=%d\n", where,
           gLastFail, static_cast<unsigned long>(gRxRawBytes),
           static_cast<unsigned>((gRingHead + RING_CAP - gRingTail) % RING_CAP),
           (int)gClosed, (int)gTerminated, gParser.statusCode());
  }

  bool gReported = false;
};

HttpTransport gHttpTransport;

Transport* httpTransport() { return &gHttpTransport; }

}  // namespace testrig
