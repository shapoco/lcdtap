#include "http_server.hpp"

#include <cstdio>
#include <cstring>

#include "lwip/tcp.h"

#include "config.h"
#include "led_status.hpp"
#include "web_assets.hpp"

// =============================================================================
// Connection state
// =============================================================================

static constexpr int MAX_CONNS = 4;
static constexpr int REQ_BUF_SIZE = 2048;
static constexpr int STAGE_SIZE = 1024;
// tcp_poll fires every ~2 s (interval 4 = 4 * 500 ms); abort after 5 quiet
// polls = ~10 s idle.
static constexpr uint8_t POLL_INTERVAL = 4;
static constexpr uint8_t POLL_LIMIT = 5;

enum class ConnState : uint8_t {
  RECV_REQ,    // accumulating request head (+ body for POST)
  SEND_FIXED,  // sending a fixed complete response (error, OPTIONS)
  SEND_ASSET,  // sending header + gzip asset body
  API_RUN,     // request handed to the JSON engine; streaming chunks
  API_END,     // terminating chunk queued; flush and close
};

struct HttpConn {
  struct tcp_pcb* pcb = nullptr;
  ConnState state = ConnState::RECV_REQ;
  uint8_t quietPolls = 0;

  char reqBuf[REQ_BUF_SIZE];
  int reqLen = 0;

  // SEND_FIXED / asset header
  char hdrBuf[256];
  int hdrLen = 0;
  int hdrPos = 0;

  // SEND_ASSET body (zero-copy from flash)
  const WebAsset* asset = nullptr;
  uint32_t assetPos = 0;

  // API_RUN: body window inside reqBuf and chunk staging
  int bodyPos = 0;
  int bodyEnd = 0;
  char stage[STAGE_SIZE];
  int stageLen = 0;
};

static HttpConn gConns[MAX_CONNS];
static JsonIntf* gApiIntf = nullptr;
static HttpConn* gApiConn = nullptr;  // the single in-flight API connection

// =============================================================================
// Helpers
// =============================================================================

static void connClose(HttpConn* c) {
  if (c->pcb) {
    tcp_arg(c->pcb, nullptr);
    tcp_recv(c->pcb, nullptr);
    tcp_sent(c->pcb, nullptr);
    tcp_err(c->pcb, nullptr);
    tcp_poll(c->pcb, nullptr, 0);
    if (tcp_close(c->pcb) != ERR_OK) {
      // Out of memory for the FIN right now; abort rather than leak.
      tcp_abort(c->pcb);
    }
    c->pcb = nullptr;
  }
  if (gApiConn == c) {
    // Dropped mid-request/response: reset the engine so a getframebuffer
    // cannot leave write protection engaged.
    jsonIntfAbort(gApiIntf);
    gApiConn = nullptr;
  }
  c->state = ConnState::RECV_REQ;
  c->reqLen = 0;
  c->stageLen = 0;
}

static void connAbandon(HttpConn* c) {
  // tcp_err already freed the pcb.
  c->pcb = nullptr;
  connClose(c);
}

// Queue a complete fixed response and switch to SEND_FIXED.
static void respondFixed(HttpConn* c, const char* status,
                         const char* extraHeaders) {
  c->hdrLen = snprintf(c->hdrBuf, sizeof(c->hdrBuf),
                       "HTTP/1.1 %s\r\n"
                       "%s"
                       "Content-Length: 0\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       status, extraHeaders ? extraHeaders : "");
  c->hdrPos = 0;
  c->state = ConnState::SEND_FIXED;
}

// Push pending header bytes; returns true when fully sent.
static bool pumpHeader(HttpConn* c) {
  while (c->hdrPos < c->hdrLen) {
    u16_t space = tcp_sndbuf(c->pcb);
    if (space == 0) return false;
    u16_t n = static_cast<u16_t>(c->hdrLen - c->hdrPos);
    if (n > space) n = space;
    if (tcp_write(c->pcb, c->hdrBuf + c->hdrPos, n, TCP_WRITE_FLAG_COPY) !=
        ERR_OK) {
      return false;
    }
    c->hdrPos += n;
  }
  return true;
}

// Advance a connection's TX state machine.
static void connPump(HttpConn* c) {
  if (!c->pcb) return;

  switch (c->state) {
    case ConnState::SEND_FIXED:
      if (pumpHeader(c)) {
        tcp_output(c->pcb);
        connClose(c);
      } else {
        tcp_output(c->pcb);
      }
      break;

    case ConnState::SEND_ASSET: {
      if (!pumpHeader(c)) {
        tcp_output(c->pcb);
        break;
      }
      // Body straight from flash: no copy, the data outlives the pcb.
      while (c->assetPos < c->asset->len) {
        u16_t space = tcp_sndbuf(c->pcb);
        if (space == 0) break;
        uint32_t rem = c->asset->len - c->assetPos;
        u16_t n = (rem > space) ? space : static_cast<u16_t>(rem);
        if (tcp_write(c->pcb, c->asset->data + c->assetPos, n, 0) != ERR_OK) {
          break;
        }
        c->assetPos += n;
        ledActivityPulse();
      }
      tcp_output(c->pcb);
      if (c->assetPos >= c->asset->len) connClose(c);
      break;
    }

    case ConnState::API_RUN:
    case ConnState::API_END:
      // Driven from httpServerProcess() via the JSON engine.
      break;

    default: break;
  }
}

// =============================================================================
// API transport (JsonIntfTransport for the HTTP JsonIntf instance)
// =============================================================================

static bool apiIsConnected(void* /*ctx*/) { return gApiConn != nullptr; }

static int apiGetChar(void* /*ctx*/) {
  HttpConn* c = gApiConn;
  if (!c || c->bodyPos >= c->bodyEnd) return -1;
  return static_cast<uint8_t>(c->reqBuf[c->bodyPos++]);
}

// Emit the staging buffer as one HTTP chunk. Returns true when the stage is
// empty afterwards.
static bool apiFlushStage(HttpConn* c) {
  if (c->stageLen == 0) return true;
  char chunkHdr[12];
  int hdrLen = snprintf(chunkHdr, sizeof(chunkHdr), "%X\r\n", c->stageLen);
  if (tcp_sndbuf(c->pcb) < static_cast<u16_t>(hdrLen + c->stageLen + 2)) {
    return false;
  }
  if (tcp_write(c->pcb, chunkHdr, static_cast<u16_t>(hdrLen),
                TCP_WRITE_FLAG_COPY) != ERR_OK) {
    return false;
  }
  tcp_write(c->pcb, c->stage, static_cast<u16_t>(c->stageLen),
            TCP_WRITE_FLAG_COPY);
  tcp_write(c->pcb, "\r\n", 2, TCP_WRITE_FLAG_COPY);
  tcp_output(c->pcb);
  c->stageLen = 0;
  ledActivityPulse();
  return true;
}

static int apiWrite(void* /*ctx*/, const char* data, int len) {
  HttpConn* c = gApiConn;
  if (!c || c->state != ConnState::API_RUN) return 0;
  int space = STAGE_SIZE - c->stageLen;
  if (space == 0) {
    // Stage full: try to push it out as a chunk; back-pressure otherwise.
    if (!apiFlushStage(c)) return 0;
    space = STAGE_SIZE;
  }
  if (len > space) len = space;
  memcpy(c->stage + c->stageLen, data, static_cast<size_t>(len));
  c->stageLen += len;
  return len;
}

// =============================================================================
// Request parsing
// =============================================================================

// Common headers for API responses. CORS is only needed when testing the
// shared web files from a local dev server; the production path (device
// serves its own page) is same-origin and ignores it.
static constexpr const char* CORS_HEADERS =
    "Access-Control-Allow-Origin: *\r\n";

static void startApiRequest(HttpConn* c, int bodyPos, int bodyEnd) {
  if (gApiConn != nullptr) {
    respondFixed(c, "503 Service Unavailable", "Retry-After: 1\r\n");
    return;
  }
  gApiConn = c;
  c->bodyPos = bodyPos;
  c->bodyEnd = bodyEnd;
  c->stageLen = 0;
  c->state = ConnState::API_RUN;

  c->hdrLen = snprintf(c->hdrBuf, sizeof(c->hdrBuf),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/json\r\n"
                       "%s"
                       "Cache-Control: no-store\r\n"
                       "Transfer-Encoding: chunked\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       CORS_HEADERS);
  c->hdrPos = 0;
}

static void startAssetResponse(HttpConn* c, const WebAsset* a) {
  c->asset = a;
  c->assetPos = 0;
  c->hdrLen = snprintf(c->hdrBuf, sizeof(c->hdrBuf),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: %s\r\n"
                       "%s"
                       "Content-Length: %lu\r\n"
                       "Cache-Control: no-cache\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       a->mime, a->gzip ? "Content-Encoding: gzip\r\n" : "",
                       static_cast<unsigned long>(a->len));
  c->hdrPos = 0;
  c->state = ConnState::SEND_ASSET;
}

static const WebAsset* findAsset(const char* path) {
  for (int i = 0; i < WEB_ASSET_COUNT; ++i) {
    if (strcmp(WEB_ASSETS[i].path, path) == 0) return &WEB_ASSETS[i];
  }
  return nullptr;
}

// Called whenever new request bytes arrived; dispatches once complete.
static void tryDispatch(HttpConn* c) {
  c->reqBuf[c->reqLen] = '\0';
  char* headEnd = strstr(c->reqBuf, "\r\n\r\n");
  if (!headEnd) {
    if (c->reqLen >= REQ_BUF_SIZE - 1) {
      respondFixed(c, "431 Request Header Fields Too Large", nullptr);
      connPump(c);
    }
    return;
  }
  const int bodyStart = static_cast<int>(headEnd - c->reqBuf) + 4;

  // Request line: METHOD SP PATH SP VERSION
  char method[8] = {};
  char path[64] = {};
  if (sscanf(c->reqBuf, "%7s %63s", method, path) != 2) {
    respondFixed(c, "400 Bad Request", nullptr);
    connPump(c);
    return;
  }

  if (strcmp(method, "OPTIONS") == 0) {
    respondFixed(c, "204 No Content",
                 "Access-Control-Allow-Origin: *\r\n"
                 "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type\r\n");
    connPump(c);
    return;
  }

  if (strcmp(method, "GET") == 0) {
    const WebAsset* a = findAsset(path);
    if (a) {
      startAssetResponse(c, a);
    } else {
      respondFixed(c, "404 Not Found", nullptr);
    }
    connPump(c);
    return;
  }

  if (strcmp(method, "POST") == 0 && strcmp(path, "/api") == 0) {
    // Wait for the whole body; commands are small (the request buffer
    // bounds them, which is fine for every command in the protocol).
    int contentLength = 0;
    // Case-insensitive header scan within the head section.
    for (char* p = c->reqBuf; p < headEnd; ++p) {
      if ((p == c->reqBuf || p[-1] == '\n') &&
          strncasecmp(p, "Content-Length:", 15) == 0) {
        contentLength = atoi(p + 15);
        break;
      }
    }
    if (contentLength < 0 || bodyStart + contentLength > REQ_BUF_SIZE - 1) {
      respondFixed(c, "413 Content Too Large", nullptr);
      connPump(c);
      return;
    }
    if (c->reqLen < bodyStart + contentLength) return;  // body incomplete
    startApiRequest(c, bodyStart, bodyStart + contentLength);
    // The 503 path queues a fixed response that nothing else would flush.
    if (c->state == ConnState::SEND_FIXED) connPump(c);
    return;
  }

  respondFixed(c, "405 Method Not Allowed", nullptr);
  connPump(c);
}

// =============================================================================
// lwIP callbacks
// =============================================================================

static err_t onRecv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
  HttpConn* c = static_cast<HttpConn*>(arg);
  if (!p) {
    // Remote closed. If we are mid-response the pump will finish and close;
    // for a half-open request just drop it.
    if (c && c->state == ConnState::RECV_REQ) connClose(c);
    return ERR_OK;
  }
  if (!c || err != ERR_OK) {
    pbuf_free(p);
    return ERR_OK;
  }

  c->quietPolls = 0;
  ledActivityPulse();

  const bool wasReceiving = (c->state == ConnState::RECV_REQ);
  if (wasReceiving) {
    const int space = REQ_BUF_SIZE - 1 - c->reqLen;
    const int n = (p->tot_len > space) ? space : p->tot_len;
    pbuf_copy_partial(p, c->reqBuf + c->reqLen, static_cast<u16_t>(n), 0);
    c->reqLen += n;
  }
  // Anything received after dispatch (pipelining) is ignored: every response
  // ends with Connection: close.

  // Release the window and the pbuf BEFORE dispatching: a small response
  // completes synchronously inside tryDispatch and closes the pcb, after
  // which it must not be touched.
  tcp_recved(pcb, p->tot_len);
  pbuf_free(p);

  if (wasReceiving) tryDispatch(c);
  return ERR_OK;
}

static err_t onSent(void* arg, struct tcp_pcb* /*pcb*/, u16_t /*len*/) {
  HttpConn* c = static_cast<HttpConn*>(arg);
  if (c) {
    c->quietPolls = 0;
    connPump(c);
  }
  return ERR_OK;
}

static void onErr(void* arg, err_t /*err*/) {
  HttpConn* c = static_cast<HttpConn*>(arg);
  if (c) connAbandon(c);
}

static err_t onPoll(void* arg, struct tcp_pcb* /*pcb*/) {
  HttpConn* c = static_cast<HttpConn*>(arg);
  if (!c) return ERR_OK;
  if (++c->quietPolls >= POLL_LIMIT) {
    // Stuck connection (client gone without RST): drop it.
    connClose(c);
    return ERR_OK;
  }
  connPump(c);
  return ERR_OK;
}

static err_t onAccept(void* /*arg*/, struct tcp_pcb* newPcb, err_t err) {
  if (err != ERR_OK || newPcb == nullptr) return ERR_VAL;

  HttpConn* c = nullptr;
  for (int i = 0; i < MAX_CONNS; ++i) {
    if (gConns[i].pcb == nullptr) {
      c = &gConns[i];
      break;
    }
  }
  if (!c) {
    tcp_abort(newPcb);
    return ERR_ABRT;
  }

  c->pcb = newPcb;
  c->state = ConnState::RECV_REQ;
  c->reqLen = 0;
  c->stageLen = 0;
  c->quietPolls = 0;
  tcp_arg(newPcb, c);
  tcp_recv(newPcb, onRecv);
  tcp_sent(newPcb, onSent);
  tcp_err(newPcb, onErr);
  tcp_poll(newPcb, onPoll, POLL_INTERVAL);
  ledActivityPulse();
  return ERR_OK;
}

// =============================================================================
// Public API
// =============================================================================

void httpServerInit(JsonIntf* apiIntf) {
  gApiIntf = apiIntf;

  struct tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
  if (!pcb) return;
  if (tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) != ERR_OK) {
    tcp_abort(pcb);
    return;
  }
  pcb = tcp_listen_with_backlog(pcb, 2);
  if (!pcb) return;
  tcp_accept(pcb, onAccept);
}

JsonIntfTransport httpServerTransport() {
  return {nullptr, apiIsConnected, apiGetChar, apiWrite};
}

void httpServerProcess() {
  HttpConn* c = gApiConn;
  if (c && c->state == ConnState::API_RUN) {
    // Response header first, then let the engine parse/stream. The engine's
    // write callback back-pressures against tcp_sndbuf via the stage.
    if (!pumpHeader(c)) {
      tcp_output(c->pcb);
      return;
    }
    jsonIntfProcess(gApiIntf);

    const bool bodyDone = c->bodyPos >= c->bodyEnd;
    if (bodyDone && jsonIntfRespIdle(gApiIntf) && apiFlushStage(c)) {
      if (tcp_sndbuf(c->pcb) >= 5 &&
          tcp_write(c->pcb, "0\r\n\r\n", 5, TCP_WRITE_FLAG_COPY) == ERR_OK) {
        tcp_output(c->pcb);
        gApiConn = nullptr;
        c->state = ConnState::API_END;
        connClose(c);
      }
    }
  }
}

bool httpServerApiBusy() { return gApiConn != nullptr; }
