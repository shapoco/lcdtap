#include "http_server.hpp"

// Placeholder: the real server lands together with the embedded web assets.

static JsonIntf* gApiIntf = nullptr;

static bool trxIsConnected(void* /*ctx*/) { return false; }
static int trxGetChar(void* /*ctx*/) { return -1; }
static int trxWrite(void* /*ctx*/, const char* /*data*/, int /*len*/) {
  return 0;
}

void httpServerInit(JsonIntf* apiIntf) { gApiIntf = apiIntf; }

JsonIntfTransport httpServerTransport() {
  return {nullptr, trxIsConnected, trxGetChar, trxWrite};
}

void httpServerProcess() {}

bool httpServerApiBusy() { return false; }
