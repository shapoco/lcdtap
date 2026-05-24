#include "uart_if.hpp"

#include <cstdio>
#include <cstring>

#include "pico/stdio.h"
#include "tusb.h"

// =============================================================================
// Module-level state
// =============================================================================

static lcdtap::LcdTap* gLcdTap = nullptr;
static InterfaceType* gCurrentIface = nullptr;
static SwitchIfaceFn gSwitchIface = nullptr;
static SaveConfigFn gSaveConfig = nullptr;

// =============================================================================
// Base64 encoder
// =============================================================================

static constexpr char kB64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64Encode3(const uint8_t in[3], char out[4]) {
  out[0] = kB64Table[in[0] >> 2];
  out[1] = kB64Table[((in[0] & 0x03u) << 4) | (in[1] >> 4)];
  out[2] = kB64Table[((in[1] & 0x0Fu) << 2) | (in[2] >> 6)];
  out[3] = kB64Table[in[2] & 0x3Fu];
}

// Encode a partial block (1 or 2 bytes) with padding.
static void b64EncodePad(const uint8_t* in, int count, char out[4]) {
  uint8_t buf[3] = {0, 0, 0};
  for (int i = 0; i < count; i++) buf[i] = in[i];
  b64Encode3(buf, out);
  if (count == 1) {
    out[2] = '=';
    out[3] = '=';
  } else if (count == 2) {
    out[3] = '=';
  }
}

// =============================================================================
// Lexer
// =============================================================================

static constexpr int TOK_MAX_LEN = 64;

enum class TokType {
  NONE,
  BRACE_OPEN,
  BRACE_CLOSE,
  COLON,
  COMMA,
  STRING,
  INTEGER,
  BOOL_TRUE,
  BOOL_FALSE,
  NULL_,
};

struct Token {
  TokType type = TokType::NONE;
  char buf[TOK_MAX_LEN + 1] = {};
  int32_t intVal = 0;
};

enum class LexState {
  IDLE,
  IN_STRING,
  IN_ESCAPE,
  IN_LITERAL,
};

struct Lexer {
  LexState state = LexState::IDLE;
  char tokBuf[TOK_MAX_LEN + 1] = {};
  int tokLen = 0;
  bool tokenReady = false;
  Token pending;
  bool crReceived = false;
  bool lineReady = false;  // CRLF received
  char deferredChar = 0;   // delimiter deferred from end of literal token

  void reset() {
    state = LexState::IDLE;
    tokLen = 0;
    tokenReady = false;
    crReceived = false;
    lineReady = false;
    deferredChar = 0;
  }
};

// Flush the accumulated literal token and return true if a token was produced.
static bool lexFlushLiteral(Lexer& lex) {
  if (lex.tokLen == 0) return false;
  lex.tokBuf[lex.tokLen] = '\0';

  Token& t = lex.pending;
  t = {};
  if (strcmp(lex.tokBuf, "true") == 0) {
    t.type = TokType::BOOL_TRUE;
  } else if (strcmp(lex.tokBuf, "false") == 0) {
    t.type = TokType::BOOL_FALSE;
  } else if (strcmp(lex.tokBuf, "null") == 0) {
    t.type = TokType::NULL_;
  } else {
    // Try integer
    t.type = TokType::INTEGER;
    int32_t val = 0;
    bool neg = false;
    int i = 0;
    if (lex.tokBuf[0] == '-') {
      neg = true;
      i = 1;
    }
    for (; i < lex.tokLen; i++) {
      char c = lex.tokBuf[i];
      if (c < '0' || c > '9') {
        t.type = TokType::NONE;
        break;
      }
      val = val * 10 + (c - '0');
    }
    t.intVal = neg ? -val : val;
  }
  lex.tokLen = 0;
  return t.type != TokType::NONE;
}

// Feed one character; returns true if a token is ready in lex.pending.
static bool lexPush(Lexer& lex, char c) {
  lex.tokenReady = false;

  // Track CRLF for end-of-command detection
  if (c == '\r') {
    lex.crReceived = true;
  } else if (c == '\n' && lex.crReceived) {
    lex.crReceived = false;
    lex.lineReady = true;
    if (lex.state == LexState::IN_LITERAL) {
      return lexFlushLiteral(lex);
    }
    return false;
  } else {
    lex.crReceived = false;
  }

  switch (lex.state) {
    case LexState::IDLE:
      if (c == '"') {
        lex.state = LexState::IN_STRING;
        lex.tokLen = 0;
      } else if (c == '{') {
        lex.pending = {};
        lex.pending.type = TokType::BRACE_OPEN;
        return true;
      } else if (c == '}') {
        lex.pending = {};
        lex.pending.type = TokType::BRACE_CLOSE;
        return true;
      } else if (c == ':') {
        lex.pending = {};
        lex.pending.type = TokType::COLON;
        return true;
      } else if (c == ',') {
        lex.pending = {};
        lex.pending.type = TokType::COMMA;
        return true;
      } else if ((c >= '0' && c <= '9') || c == '-') {
        lex.state = LexState::IN_LITERAL;
        lex.tokLen = 0;
        lex.tokBuf[lex.tokLen++] = c;
      } else if (c == 't' || c == 'f' || c == 'n') {
        lex.state = LexState::IN_LITERAL;
        lex.tokLen = 0;
        lex.tokBuf[lex.tokLen++] = c;
      }
      break;

    case LexState::IN_STRING:
      if (c == '\\') {
        lex.state = LexState::IN_ESCAPE;
      } else if (c == '"') {
        lex.state = LexState::IDLE;
        lex.tokBuf[lex.tokLen] = '\0';
        lex.pending = {};
        lex.pending.type = TokType::STRING;
        memcpy(lex.pending.buf, lex.tokBuf,
               static_cast<size_t>(lex.tokLen + 1));
        lex.tokLen = 0;
        return true;
      } else {
        if (lex.tokLen < TOK_MAX_LEN) lex.tokBuf[lex.tokLen++] = c;
      }
      break;

    case LexState::IN_ESCAPE:
      if (lex.tokLen < TOK_MAX_LEN) {
        // Only handle basic escapes
        switch (c) {
          case '"':
          case '\\':
          case '/': lex.tokBuf[lex.tokLen++] = c; break;
          case 'n': lex.tokBuf[lex.tokLen++] = '\n'; break;
          case 'r': lex.tokBuf[lex.tokLen++] = '\r'; break;
          case 't': lex.tokBuf[lex.tokLen++] = '\t'; break;
          default: lex.tokBuf[lex.tokLen++] = c; break;
        }
      }
      lex.state = LexState::IN_STRING;
      break;

    case LexState::IN_LITERAL: {
      bool isLiteralChar = (c >= '0' && c <= '9') || c == '-' ||
                           (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      if (isLiteralChar) {
        if (lex.tokLen < TOK_MAX_LEN) lex.tokBuf[lex.tokLen++] = c;
      } else {
        // End of literal — flush it; save the delimiter for next call to
        // avoid overwriting lex.pending with the delimiter token.
        lex.state = LexState::IDLE;
        bool had = lexFlushLiteral(lex);
        if (had) {
          lex.deferredChar = c;
          return true;
        }
        return lexPush(lex, c);
      }
      break;
    }
  }
  return false;
}

// =============================================================================
// Parser
// =============================================================================

enum class ParseState {
  EXPECT_OBJ_OPEN,
  EXPECT_TOP_KEY_OR_CLOSE,
  EXPECT_CMD_COLON,
  EXPECT_CMD_VALUE,
  EXPECT_PARAMS_COLON,
  EXPECT_PARAMS_OBJ_OPEN,
  EXPECT_PARAM_KEY_OR_CLOSE,
  EXPECT_PARAM_COLON,
  EXPECT_PARAM_VALUE,
  EXPECT_AFTER_PARAM,
  EXPECT_AFTER_TOP_KV,
  CMD_READY,
  ERROR,
};

static constexpr int MAX_PARAMS = 16;

struct Param {
  char key[TOK_MAX_LEN + 1];
  int32_t value;
};

struct Parser {
  ParseState state = ParseState::EXPECT_OBJ_OPEN;
  char command[TOK_MAX_LEN + 1] = {};
  Param params[MAX_PARAMS] = {};
  int numParams = 0;
  bool hasParams = false;

  void reset() {
    state = ParseState::EXPECT_OBJ_OPEN;
    command[0] = '\0';
    numParams = 0;
    hasParams = false;
  }
};

// Feed one token to the parser. Returns true when CMD_READY or ERROR.
static bool parserFeed(Parser& p, const Token& tok) {
  switch (p.state) {
    case ParseState::EXPECT_OBJ_OPEN:
      if (tok.type == TokType::BRACE_OPEN) {
        p.state = ParseState::EXPECT_TOP_KEY_OR_CLOSE;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_TOP_KEY_OR_CLOSE:
      if (tok.type == TokType::BRACE_CLOSE) {
        p.state = ParseState::CMD_READY;
        return true;
      } else if (tok.type == TokType::STRING) {
        if (strcmp(tok.buf, "command") == 0) {
          p.state = ParseState::EXPECT_CMD_COLON;
        } else if (strcmp(tok.buf, "params") == 0) {
          p.state = ParseState::EXPECT_PARAMS_COLON;
        } else {
          p.state = ParseState::ERROR;
          return true;
        }
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_CMD_COLON:
      if (tok.type == TokType::COLON) {
        p.state = ParseState::EXPECT_CMD_VALUE;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_CMD_VALUE:
      if (tok.type == TokType::STRING) {
        strncpy(p.command, tok.buf, TOK_MAX_LEN);
        p.command[TOK_MAX_LEN] = '\0';
        p.state = ParseState::EXPECT_AFTER_TOP_KV;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_PARAMS_COLON:
      if (tok.type == TokType::COLON) {
        p.state = ParseState::EXPECT_PARAMS_OBJ_OPEN;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_PARAMS_OBJ_OPEN:
      if (tok.type == TokType::BRACE_OPEN) {
        p.hasParams = true;
        p.state = ParseState::EXPECT_PARAM_KEY_OR_CLOSE;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_PARAM_KEY_OR_CLOSE:
      if (tok.type == TokType::BRACE_CLOSE) {
        p.state = ParseState::EXPECT_AFTER_TOP_KV;
      } else if (tok.type == TokType::STRING) {
        if (p.numParams < MAX_PARAMS) {
          strncpy(p.params[p.numParams].key, tok.buf, TOK_MAX_LEN);
          p.params[p.numParams].key[TOK_MAX_LEN] = '\0';
        }
        p.state = ParseState::EXPECT_PARAM_COLON;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_PARAM_COLON:
      if (tok.type == TokType::COLON) {
        p.state = ParseState::EXPECT_PARAM_VALUE;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_PARAM_VALUE:
      if (tok.type == TokType::INTEGER) {
        if (p.numParams < MAX_PARAMS) {
          p.params[p.numParams].value = tok.intVal;
          p.numParams++;
        }
        p.state = ParseState::EXPECT_AFTER_PARAM;
      } else if (tok.type == TokType::BOOL_TRUE) {
        if (p.numParams < MAX_PARAMS) {
          p.params[p.numParams].value = 1;
          p.numParams++;
        }
        p.state = ParseState::EXPECT_AFTER_PARAM;
      } else if (tok.type == TokType::BOOL_FALSE) {
        if (p.numParams < MAX_PARAMS) {
          p.params[p.numParams].value = 0;
          p.numParams++;
        }
        p.state = ParseState::EXPECT_AFTER_PARAM;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_AFTER_PARAM:
      if (tok.type == TokType::COMMA) {
        p.state = ParseState::EXPECT_PARAM_KEY_OR_CLOSE;
      } else if (tok.type == TokType::BRACE_CLOSE) {
        p.state = ParseState::EXPECT_AFTER_TOP_KV;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_AFTER_TOP_KV:
      if (tok.type == TokType::COMMA) {
        p.state = ParseState::EXPECT_TOP_KEY_OR_CLOSE;
      } else if (tok.type == TokType::BRACE_CLOSE) {
        p.state = ParseState::CMD_READY;
        return true;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::CMD_READY:
    case ParseState::ERROR: break;
  }
  return p.state == ParseState::CMD_READY || p.state == ParseState::ERROR;
}

// =============================================================================
// Response generator
// =============================================================================

enum class RespPhase {
  IDLE,
  SHORT,        // sending a fixed response string
  PARAMS,       // getparams: stream one param object at a time
  FB_HEADER,    // getframebuffer: initial JSON fragment
  FB_DATA,      // getframebuffer: base64 pixel stream
  FB_FOOTER,    // getframebuffer: closing fragment
  DUMP_HEADER,  // cmddump_read: initial JSON fragment
  DUMP_DATA,    // cmddump_read: base64 data stream
  DUMP_FOOTER,  // cmddump_read: closing fragment
};

struct RespGen {
  RespPhase phase = RespPhase::IDLE;

  // SHORT / HEADER / FOOTER: pointer + remaining length into a static string
  const char* strPtr = nullptr;
  int strRem = 0;

  // PARAMS: which parameter index is being emitted
  int paramIdx = 0;

  // FB_DATA / DUMP_DATA: pixel/byte position and base64 accumulator
  uint16_t fbOutX = 0;
  uint16_t fbOutY = 0;
  uint16_t fbPhysW = 0;
  uint16_t fbPhysH = 0;
  uint16_t fbOutW = 0;
  uint16_t fbOutH = 0;
  uint8_t fbRot = 0;
  bool fbInverted = false;
  const uint16_t* fbPtr = nullptr;

  // Shared base64 accumulator for framebuffer and dump streams
  uint8_t b64Acc[3] = {};
  int b64Count = 0;  // bytes in b64Acc
  char b64Out[4] = {};
  int b64OutPos = 0;  // chars already sent from b64Out

  // Dump stream
  const uint16_t* dumpPtr = nullptr;
  uint16_t dumpLen = 0;
  uint16_t dumpPos = 0;
  bool dumpHighByte = false;  // true = next byte to send is high byte

  // Per-call chunk buffer for formatted segments
  char chunkBuf[192] = {};
  int chunkLen = 0;
  int chunkPos = 0;
};

static RespGen gResp;

// -----------------------------------------------------------------------------
// USB CDC non-blocking write helpers
// -----------------------------------------------------------------------------

// Write as many bytes as CDC TX buffer has room for. Returns bytes written.
static uint32_t cdcWrite(const char* buf, uint32_t len) {
  if (!tud_cdc_connected()) return 0;
  uint32_t avail = tud_cdc_write_available();
  if (avail == 0) return 0;
  uint32_t n = len < avail ? len : avail;
  uint32_t written = tud_cdc_write(buf, n);
  if (written > 0) tud_cdc_write_flush();
  return written;
}

// Attempt to drain gResp.chunkBuf. Returns true when fully drained.
static bool drainChunk() {
  while (gResp.chunkPos < gResp.chunkLen) {
    uint32_t rem = static_cast<uint32_t>(gResp.chunkLen - gResp.chunkPos);
    uint32_t sent = cdcWrite(gResp.chunkBuf + gResp.chunkPos, rem);
    if (sent == 0) return false;  // TX buffer full — retry next call
    gResp.chunkPos += static_cast<int>(sent);
  }
  gResp.chunkLen = 0;
  gResp.chunkPos = 0;
  return true;
}

// Load a string into chunkBuf.
static void chunkFromStr(const char* s) {
  int len = static_cast<int>(strlen(s));
  if (len > static_cast<int>(sizeof(gResp.chunkBuf) - 1))
    len = static_cast<int>(sizeof(gResp.chunkBuf) - 1);
  memcpy(gResp.chunkBuf, s, static_cast<size_t>(len));
  gResp.chunkLen = len;
  gResp.chunkPos = 0;
}

// =============================================================================
// getparams output helpers
// =============================================================================

static constexpr int NUM_PARAMS_OUT = 8;

static const char* kControllerOptions[] = {"ST7789", "SSD1306", "SSD1331"};
static const char* kIfaceOptions[] = {"I2C", "4-Line SPI", "3-Line SPI",
                                      "8-bit Parallel"};
static const char* kRotOptions[] = {"0", "90", "180", "270"};

// Build the JSON fragment for parameter at index idx into chunkBuf.
// Returns false when idx is out of range.
static bool buildParamChunk(int idx, const lcdtap::LcdTapConfig& cfg,
                            InterfaceType iface) {
  char* buf = gResp.chunkBuf;
  int cap = static_cast<int>(sizeof(gResp.chunkBuf));
  int n = 0;
  bool first = (idx == 0);
  bool last = (idx == NUM_PARAMS_OUT - 1);
  const char* prefix = first ? "{\"params\":[" : "";
  const char* suffix = last ? "]}\r\n" : ",";

  switch (idx) {
    case 0:  // controller ENUM
      n = snprintf(buf, static_cast<size_t>(cap),
                   "%s{\"id\":\"controller\",\"type\":\"ENUM\","
                   "\"name\":\"Controller Type\",\"unit\":null,"
                   "\"options\":{\"ST7789\":0,\"SSD1306\":1,\"SSD1331\":2},"
                   "\"value\":%d}%s",
                   prefix, static_cast<int>(cfg.controller), suffix);
      break;
    case 1:  // interfaceType ENUM
      n = snprintf(buf, static_cast<size_t>(cap),
                   "%s{\"id\":\"interfaceType\",\"type\":\"ENUM\","
                   "\"name\":\"Interface Type\",\"unit\":null,"
                   "\"options\":{\"I2C\":0,\"4-Line SPI\":1,"
                   "\"3-Line SPI\":2,\"8-bit Parallel\":3},"
                   "\"value\":%d}%s",
                   prefix, static_cast<int>(iface), suffix);
      (void)kIfaceOptions;
      break;
    case 2:  // lcdWidth INTEGER
      n = snprintf(buf, static_cast<size_t>(cap),
                   "%s{\"id\":\"lcdWidth\",\"type\":\"INTEGER\","
                   "\"name\":\"LCD Width\",\"unit\":\"px\","
                   "\"min\":32,\"max\":480,\"step\":8,"
                   "\"value\":%d}%s",
                   prefix, static_cast<int>(cfg.lcdWidth), suffix);
      break;
    case 3:  // lcdHeight INTEGER
      n = snprintf(buf, static_cast<size_t>(cap),
                   "%s{\"id\":\"lcdHeight\",\"type\":\"INTEGER\","
                   "\"name\":\"LCD Height\",\"unit\":\"px\","
                   "\"min\":32,\"max\":480,\"step\":8,"
                   "\"value\":%d}%s",
                   prefix, static_cast<int>(cfg.lcdHeight), suffix);
      break;
    case 4:  // inverted BOOLEAN
      n = snprintf(buf, static_cast<size_t>(cap),
                   "%s{\"id\":\"inverted\",\"type\":\"BOOLEAN\","
                   "\"name\":\"Inverted\","
                   "\"value\":%s}%s",
                   prefix, cfg.inverted ? "true" : "false", suffix);
      break;
    case 5:  // swapRB BOOLEAN
      n = snprintf(buf, static_cast<size_t>(cap),
                   "%s{\"id\":\"swapRB\",\"type\":\"BOOLEAN\","
                   "\"name\":\"Swap R/B\","
                   "\"value\":%s}%s",
                   prefix, cfg.swapRB ? "true" : "false", suffix);
      break;
    case 6:  // outputRotation ENUM
      n = snprintf(buf, static_cast<size_t>(cap),
                   "%s{\"id\":\"outputRotation\",\"type\":\"ENUM\","
                   "\"name\":\"Output Rotation\",\"unit\":\"deg\","
                   "\"options\":{\"0\":0,\"90\":1,\"180\":2,\"270\":3},"
                   "\"value\":%d}%s",
                   prefix, static_cast<int>(cfg.outputRotation), suffix);
      (void)kRotOptions;
      break;
    case 7:  // forcePowerOn BOOLEAN
      n = snprintf(buf, static_cast<size_t>(cap),
                   "%s{\"id\":\"forcePowerOn\",\"type\":\"BOOLEAN\","
                   "\"name\":\"Force Power On\","
                   "\"value\":%s}%s",
                   prefix, cfg.forcePowerOn ? "true" : "false", suffix);
      break;
    default: (void)kControllerOptions; return false;
  }
  gResp.chunkLen = n < cap ? n : cap - 1;
  gResp.chunkPos = 0;
  return true;
}

// =============================================================================
// Command execution
// =============================================================================

static void respSetShort(const char* s) {
  gResp.phase = RespPhase::SHORT;
  chunkFromStr(s);
}

static void execCommand(const Parser& p) {
  const char* cmd = p.command;

  // ----- hello -----
  if (strcmp(cmd, "hello") == 0) {
    respSetShort("{\"response\":\"welcome lcdtap\"}\r\n");
    return;
  }

  // ----- getparams -----
  if (strcmp(cmd, "getparams") == 0) {
    gResp.phase = RespPhase::PARAMS;
    gResp.paramIdx = 0;
    gResp.chunkLen = 0;
    gResp.chunkPos = 0;
    return;
  }

  // ----- setparams -----
  if (strcmp(cmd, "setparams") == 0) {
    lcdtap::LcdTapConfig cfg = gLcdTap->getConfig();
    InterfaceType newIface = *gCurrentIface;

    for (int i = 0; i < p.numParams; i++) {
      const char* k = p.params[i].key;
      int32_t v = p.params[i].value;
      if (strcmp(k, "controller") == 0) {
        cfg.controller = static_cast<lcdtap::ControllerType>(v);
      } else if (strcmp(k, "interfaceType") == 0) {
        newIface = static_cast<InterfaceType>(v);
      } else if (strcmp(k, "lcdWidth") == 0) {
        cfg.lcdWidth = static_cast<uint16_t>(v);
      } else if (strcmp(k, "lcdHeight") == 0) {
        cfg.lcdHeight = static_cast<uint16_t>(v);
      } else if (strcmp(k, "inverted") == 0) {
        cfg.inverted = (v != 0);
      } else if (strcmp(k, "swapRB") == 0) {
        cfg.swapRB = (v != 0);
      } else if (strcmp(k, "outputRotation") == 0) {
        cfg.outputRotation = static_cast<uint8_t>(v);
      } else if (strcmp(k, "forcePowerOn") == 0) {
        cfg.forcePowerOn = (v != 0);
      }
    }

    lcdtap::Status st = gLcdTap->updateConfig(cfg);
    if (st != lcdtap::Status::OK) {
      respSetShort("{\"error\":\"updateConfig failed\"}\r\n");
      return;
    }

    if (newIface != *gCurrentIface) {
      gSwitchIface(newIface);
    }

    ConfigFile toSave;
    toSave.libConfig = gLcdTap->getConfig();
    toSave.interfaceType = newIface;
    gSaveConfig(toSave);

    respSetShort("{\"response\":\"ok\"}\r\n");
    return;
  }

  // ----- getframebuffer -----
  if (strcmp(cmd, "getframebuffer") == 0) {
    gLcdTap->setWriteProtected(true);

    lcdtap::LcdTapConfig cfg = gLcdTap->getConfig();
    uint16_t physW = cfg.lcdWidth;
    uint16_t physH = cfg.lcdHeight;
    uint8_t rot = cfg.outputRotation & 3u;

    gResp.fbPhysW = physW;
    gResp.fbPhysH = physH;
    gResp.fbRot = rot;
    gResp.fbInverted = gLcdTap->isOutputInverted();
    gResp.fbPtr = gLcdTap->getFramebuf();
    if ((rot & 1u) == 0u) {
      gResp.fbOutW = physW;
      gResp.fbOutH = physH;
    } else {
      gResp.fbOutW = physH;
      gResp.fbOutH = physW;
    }
    gResp.fbOutX = 0;
    gResp.fbOutY = 0;
    gResp.b64Count = 0;
    gResp.b64OutPos = 4;  // force next chunk

    // Build header string into chunkBuf
    snprintf(gResp.chunkBuf, sizeof(gResp.chunkBuf),
             "{\"width\":%d,\"height\":%d,\"format\":\"RGB565\",\"data\":\"",
             static_cast<int>(gResp.fbOutW), static_cast<int>(gResp.fbOutH));
    gResp.chunkLen = static_cast<int>(strlen(gResp.chunkBuf));
    gResp.chunkPos = 0;
    gResp.phase = RespPhase::FB_HEADER;
    return;
  }

  // ----- cmddump_start -----
  if (strcmp(cmd, "cmddump_start") == 0) {
    gLcdTap->dumpStart(lcdtap::getDefaultDumpConfig());
    respSetShort("{\"response\":\"ok\"}\r\n");
    return;
  }

  // ----- cmddump_abort -----
  if (strcmp(cmd, "cmddump_abort") == 0) {
    gLcdTap->dumpAbort();
    respSetShort("{\"response\":\"ok\"}\r\n");
    return;
  }

  // ----- cmddump_forcetrigger -----
  if (strcmp(cmd, "cmddump_forcetrigger") == 0) {
    gLcdTap->dumpForceTrigger();
    respSetShort("{\"response\":\"ok\"}\r\n");
    return;
  }

  // ----- cmddump_getstatus -----
  if (strcmp(cmd, "cmddump_getstatus") == 0) {
    const char* stStr = "WAIT";
    switch (gLcdTap->dumpGetState()) {
      case lcdtap::DumpState::WAIT: stStr = "WAIT"; break;
      case lcdtap::DumpState::ACTIVE: stStr = "ACTIVE"; break;
      case lcdtap::DumpState::COMPLETE: stStr = "COMPLETE"; break;
    }
    uint16_t dumpSize = gLcdTap->dumpGetSize();
    snprintf(gResp.chunkBuf, sizeof(gResp.chunkBuf),
             "{\"status\":\"%s\",\"bytes\":%d}\r\n", stStr,
             static_cast<int>(dumpSize) * 2);
    gResp.chunkLen = static_cast<int>(strlen(gResp.chunkBuf));
    gResp.chunkPos = 0;
    gResp.phase = RespPhase::SHORT;
    return;
  }

  // ----- cmddump_read -----
  if (strcmp(cmd, "cmddump_read") == 0) {
    uint16_t dlen = gLcdTap->dumpGetSize();
    gResp.dumpPtr = gLcdTap->dumpGetBuffer();
    gResp.dumpLen = dlen;
    gResp.dumpPos = 0;
    gResp.dumpHighByte = false;
    gResp.b64Count = 0;
    gResp.b64OutPos = 4;

    snprintf(gResp.chunkBuf, sizeof(gResp.chunkBuf),
             "{\"length\":%d,\"data\":\"", static_cast<int>(dlen));
    gResp.chunkLen = static_cast<int>(strlen(gResp.chunkBuf));
    gResp.chunkPos = 0;
    gResp.phase = RespPhase::DUMP_HEADER;
    return;
  }

  // ----- unknown -----
  respSetShort("{\"error\":\"unknown command\"}\r\n");
}

// =============================================================================
// Response flushing
// =============================================================================

// Map output pixel coordinate (dx, dy) to framebuf index.
static inline uint32_t fbIndex(uint16_t dx, uint16_t dy, uint16_t physW,
                               uint16_t physH, uint8_t rot) {
  switch (rot) {
    default:
    case 0: return static_cast<uint32_t>(dy) * physW + dx;
    case 1: return static_cast<uint32_t>(physH - 1u - dx) * physW + dy;
    case 2:
      return static_cast<uint32_t>(physH - 1u - dy) * physW + (physW - 1u - dx);
    case 3: return static_cast<uint32_t>(dx) * physW + (physW - 1u - dy);
  }
}

// Feed one byte into the base64 accumulator; if a full 3-byte block is ready
// encode it and store 4 chars into b64Out.
static bool b64Feed(uint8_t byte) {
  gResp.b64Acc[gResp.b64Count++] = byte;
  if (gResp.b64Count == 3) {
    b64Encode3(gResp.b64Acc, gResp.b64Out);
    gResp.b64Count = 0;
    gResp.b64OutPos = 0;
    return true;
  }
  return false;
}

// Flush pending base64 output chars. Returns true when flushed (or nothing
// pending).
static bool b64DrainOut() {
  while (gResp.b64OutPos < 4) {
    uint32_t sent = cdcWrite(gResp.b64Out + gResp.b64OutPos,
                             static_cast<uint32_t>(4 - gResp.b64OutPos));
    if (sent == 0) return false;
    gResp.b64OutPos += static_cast<int>(sent);
  }
  return true;
}

// Flush the base64 padding at stream end. Returns true when done.
static bool b64FlushPad() {
  if (gResp.b64Count == 0) return true;
  b64EncodePad(gResp.b64Acc, gResp.b64Count, gResp.b64Out);
  gResp.b64Count = 0;
  gResp.b64OutPos = 0;
  return b64DrainOut();
}

// Advance the response generator state machine; call repeatedly until IDLE.
static void respFlush() {
  switch (gResp.phase) {
    case RespPhase::IDLE: break;

    case RespPhase::SHORT:
      if (drainChunk()) gResp.phase = RespPhase::IDLE;
      break;

    case RespPhase::PARAMS: {
      if (!drainChunk()) break;
      lcdtap::LcdTapConfig cfg = gLcdTap->getConfig();
      if (!buildParamChunk(gResp.paramIdx, cfg, *gCurrentIface)) {
        gResp.phase = RespPhase::IDLE;
        break;
      }
      gResp.paramIdx++;
      // drainChunk will be called on the next respFlush() invocation
      break;
    }

    case RespPhase::FB_HEADER:
      if (drainChunk()) gResp.phase = RespPhase::FB_DATA;
      break;

    case RespPhase::FB_DATA: {
      // First drain any pending base64 output chars
      if (!b64DrainOut()) break;

      if (gResp.fbPtr == nullptr || gResp.fbOutY >= gResp.fbOutH) {
        // All pixels done — flush padding
        if (!b64FlushPad()) break;
        chunkFromStr("\"}\r\n");
        gResp.phase = RespPhase::FB_FOOTER;
        gLcdTap->setWriteProtected(false);
        break;
      }

      // Process pixels until CDC is full or end of row.
      // Advance fbOutX/fbOutY BEFORE b64Feed so a CDC-full retry does not
      // re-feed the same pixel a second time.
      while (gResp.fbOutY < gResp.fbOutH) {
        if (!b64DrainOut()) break;  // drain before reading next pixel

        uint32_t idx = fbIndex(gResp.fbOutX, gResp.fbOutY, gResp.fbPhysW,
                               gResp.fbPhysH, gResp.fbRot);
        uint16_t px = gResp.fbPtr[idx];
        if (gResp.fbInverted) px ^= 0xFFFFu;

        // Advance position before feeding bytes
        bool endOfRow = (++gResp.fbOutX >= gResp.fbOutW);
        if (endOfRow) {
          gResp.fbOutX = 0;
          gResp.fbOutY++;
        }

        // Little-endian: low byte first.
        // Per pixel at most one b64Feed call returns true (low XOR high, never
        // both), so draining after low does not discard the high byte.
        // Always feed high regardless of whether low triggered a drain —
        // breaking after low would leave the high byte undelivered.
        if (b64Feed(static_cast<uint8_t>(px & 0xFFu))) b64DrainOut();
        if (b64Feed(static_cast<uint8_t>(px >> 8)) && !b64DrainOut()) break;

        if (endOfRow) break;  // yield once per row to keep IRQ latency low
      }
      break;
    }

    case RespPhase::FB_FOOTER:
      if (drainChunk()) gResp.phase = RespPhase::IDLE;
      break;

    case RespPhase::DUMP_HEADER:
      if (drainChunk()) gResp.phase = RespPhase::DUMP_DATA;
      break;

    case RespPhase::DUMP_DATA: {
      if (!b64DrainOut()) break;

      if (gResp.dumpPos >= gResp.dumpLen) {
        if (!b64FlushPad()) break;
        chunkFromStr("\"}\r\n");
        gResp.phase = RespPhase::DUMP_FOOTER;
        break;
      }

      // One uint16_t = 2 bytes (little-endian).
      // Advance dumpPos BEFORE b64Feed so a CDC-full retry does not re-feed
      // the same word.
      // Per word at most one b64Feed returns true (low XOR high, never both),
      // so draining after low is safe and we must always feed high regardless.
      uint16_t word = gResp.dumpPtr[gResp.dumpPos++];
      if (b64Feed(static_cast<uint8_t>(word & 0xFFu))) b64DrainOut();
      if (b64Feed(static_cast<uint8_t>(word >> 8))) b64DrainOut();
      break;
    }

    case RespPhase::DUMP_FOOTER:
      if (drainChunk()) gResp.phase = RespPhase::IDLE;
      break;
  }
}

// =============================================================================
// RX processing
// =============================================================================

static Lexer gLex;
static Parser gParser;

static const char* parseStateStr(ParseState s) {
  switch (s) {
    case ParseState::EXPECT_OBJ_OPEN: return "EXPECT_OBJ_OPEN";
    case ParseState::EXPECT_TOP_KEY_OR_CLOSE: return "EXPECT_TOP_KEY";
    case ParseState::EXPECT_CMD_COLON: return "EXPECT_CMD_COLON";
    case ParseState::EXPECT_CMD_VALUE: return "EXPECT_CMD_VALUE";
    case ParseState::EXPECT_PARAMS_COLON: return "EXPECT_PARAMS_COLON";
    case ParseState::EXPECT_PARAMS_OBJ_OPEN: return "EXPECT_PARAMS_OBJ";
    case ParseState::EXPECT_PARAM_KEY_OR_CLOSE: return "EXPECT_PARAM_KEY";
    case ParseState::EXPECT_PARAM_COLON: return "EXPECT_PARAM_COLON";
    case ParseState::EXPECT_PARAM_VALUE: return "EXPECT_PARAM_VALUE";
    case ParseState::EXPECT_AFTER_PARAM: return "EXPECT_AFTER_PARAM";
    case ParseState::EXPECT_AFTER_TOP_KV: return "EXPECT_AFTER_TOP_KV";
    default: return "UNKNOWN";
  }
}

static void processRxChar(char c) {
  bool tokReady = lexPush(gLex, c);

  if (tokReady) {
    bool done = parserFeed(gParser, gLex.pending);
    if (done) {
      if (gParser.state == ParseState::CMD_READY) {
        execCommand(gParser);
      } else {
        snprintf(gResp.chunkBuf, sizeof(gResp.chunkBuf),
                 "{\"error\":\"parse error\",\"state\":\"%s\"}\r\n",
                 parseStateStr(gParser.state));
        gResp.chunkLen = static_cast<int>(strlen(gResp.chunkBuf));
        gResp.chunkPos = 0;
        gResp.phase = RespPhase::SHORT;
      }
      gParser.reset();
      gLex.reset();
    }
  }

  // If CRLF arrived but no token, treat as end-of-command attempt
  if (gLex.lineReady) {
    gLex.lineReady = false;
    if (gParser.state != ParseState::EXPECT_OBJ_OPEN) {
      // Incomplete command — reset quietly
      gParser.reset();
      gLex.reset();
    }
  }

  // Process delimiter deferred from end of a literal token
  if (gLex.deferredChar) {
    char dc = gLex.deferredChar;
    gLex.deferredChar = 0;
    processRxChar(dc);
  }
}

// =============================================================================
// Public API
// =============================================================================

void uartIfInit(lcdtap::LcdTap* lcdtap, InterfaceType* currentIface,
                SwitchIfaceFn switchIface, SaveConfigFn saveConfig) {
  gLcdTap = lcdtap;
  gCurrentIface = currentIface;
  gSwitchIface = switchIface;
  gSaveConfig = saveConfig;

  stdio_init_all();

  gLex.reset();
  gParser.reset();
  gResp = {};
}

void uartIfProcess() {
  if (!tud_cdc_connected()) return;

  // Receive up to a small burst of characters per call to avoid starving
  // other main-loop work.
  for (int i = 0; i < 64; i++) {
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT) break;
    processRxChar(static_cast<char>(c));
  }

  respFlush();
}
