#include "testrig/jsonclient.hpp"

#include <cstdio>
#include <cstring>

namespace testrig {

bool JsonClient::send(const char* cmdLine, uint32_t timeoutMs) {
  if (!t_.connected()) return false;
  // A stale partial response would desync the parser; start clean.
  t_.drainInput();
  size_t len = strlen(cmdLine);
  if (!t_.sendLine(cmdLine, len, timeoutMs)) return false;
  return t_.sendLine("\r\n", 2, timeoutMs);
}

int JsonClient::readLine(uint32_t timeoutMs) {
  size_t pos = 0;
  while (true) {
    int c = t_.recvChar(timeoutMs);
    if (c < 0) return -1;
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf_[pos] = '\0';
      return static_cast<int>(pos);
    }
    if (pos + 1 < LINE_CAP) lineBuf_[pos++] = static_cast<char>(c);
    // Overlong lines are truncated; the JSON parse below then fails cleanly.
  }
}

bool JsonClient::expect(const char* s, uint32_t timeoutMs) {
  for (const char* p = s; *p != '\0'; p++) {
    int c = t_.recvChar(timeoutMs);
    if (c < 0 || c != *p) return false;
  }
  return true;
}

bool JsonClient::readUint(uint32_t* value, char delim, uint32_t timeoutMs) {
  uint32_t v = 0;
  bool any = false;
  while (true) {
    int c = t_.recvChar(timeoutMs);
    if (c < 0) return false;
    if (c == delim) {
      *value = v;
      return any;
    }
    if (c < '0' || c > '9') return false;
    v = v * 10u + static_cast<uint32_t>(c - '0');
    any = true;
  }
}

bool JsonClient::hello(uint32_t timeoutMs) {
  JsonDocument doc;
  if (!command("{\"command\":\"hello\"}", doc, timeoutMs)) return false;
  const char* resp = doc["response"];
  return resp != nullptr && strcmp(resp, "welcome lcdtap") == 0;
}

bool JsonClient::command(const char* cmdLine, JsonDocument& doc,
                         uint32_t timeoutMs) {
  if (!send(cmdLine, timeoutMs)) return false;
  // Skip anything that is not a JSON object line (e.g. stray log output).
  for (int attempt = 0; attempt < 8; attempt++) {
    int n = readLine(timeoutMs);
    if (n < 0) return false;
    if (n == 0 || lineBuf_[0] != '{') continue;
    return deserializeJson(doc, lineBuf_, static_cast<size_t>(n)) ==
           DeserializationError::Ok;
  }
  return false;
}

bool JsonClient::getFramebuffer(FrameVerifier& v, bool writeProtected,
                                uint32_t timeoutMs) {
  char cmd[96];
  snprintf(cmd, sizeof(cmd),
           "{\"command\":\"getframebuffer\",\"writeProtected\":%s}",
           writeProtected ? "true" : "false");
  if (!send(cmd, timeoutMs)) return false;

  // The response header has a fixed shape (json_intf.cpp emits it with a
  // single snprintf), so a literal match is exact, not fragile:
  //   {"width":W,"height":H,"format":"RGB565-RLE","data":"...."}
  // An {"error":...} line is the only other possibility.
  int c = t_.recvChar(timeoutMs);
  while (c == '\r' || c == '\n') c = t_.recvChar(timeoutMs);
  if (c != '{') return false;
  if (!expect("\"", timeoutMs)) return false;
  c = t_.recvChar(timeoutMs);
  if (c != 'w') {
    // Probably an error response; consume the rest of the line and fail.
    while (c >= 0 && c != '\n') c = t_.recvChar(timeoutMs);
    return false;
  }
  if (!expect("idth\":", timeoutMs)) return false;
  uint32_t w = 0, h = 0;
  if (!readUint(&w, ',', timeoutMs)) return false;
  if (!expect("\"height\":", timeoutMs)) return false;
  if (!readUint(&h, ',', timeoutMs)) return false;
  if (!expect("\"format\":\"RGB565-RLE\",\"data\":\"", timeoutMs)) {
    return false;
  }

  bool dimsOk = (w == v.expectedWidth() && h == v.expectedHeight());

  // Stream the Base64 payload into the verifier up to the closing quote.
  while (true) {
    c = t_.recvChar(timeoutMs);
    if (c < 0) return false;
    if (c == '"') break;
    if (dimsOk) v.feedBase64(static_cast<char>(c));
  }
  // Trailing "}" + CRLF.
  while (c >= 0 && c != '\n') c = t_.recvChar(timeoutMs);
  return dimsOk;
}

bool JsonClient::getTextBuffer(uint16_t* cols, uint16_t* rows, uint8_t* out,
                               size_t cap, uint32_t timeoutMs) {
  JsonDocument doc;
  if (!command("{\"command\":\"gettextbuffer\"}", doc, timeoutMs)) {
    return false;
  }
  if (doc["cols"].isNull() || doc["rows"].isNull()) return false;
  *cols = doc["cols"].as<uint16_t>();
  *rows = doc["rows"].as<uint16_t>();
  const char* b64 = doc["data"];
  if (b64 == nullptr) return false;

  // Decode Base64 (payload is at most 160 bytes).
  size_t need = static_cast<size_t>(*cols) * (*rows);
  size_t outLen = 0;
  uint32_t acc = 0;
  int accBits = 0;
  for (const char* p = b64; *p != '\0'; p++) {
    char ch = *p;
    int val;
    if (ch >= 'A' && ch <= 'Z') {
      val = ch - 'A';
    } else if (ch >= 'a' && ch <= 'z') {
      val = ch - 'a' + 26;
    } else if (ch >= '0' && ch <= '9') {
      val = ch - '0' + 52;
    } else if (ch == '+') {
      val = 62;
    } else if (ch == '/') {
      val = 63;
    } else if (ch == '=') {
      break;
    } else {
      return false;
    }
    acc = (acc << 6) | static_cast<uint32_t>(val);
    accBits += 6;
    if (accBits >= 8) {
      accBits -= 8;
      if (outLen < cap) out[outLen++] = static_cast<uint8_t>(acc >> accBits);
    }
  }
  return outLen >= need || need > cap;
}

}  // namespace testrig
