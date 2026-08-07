#pragma once

// JSON command interface engine.
//
// Receives newline-terminated JSON command objects from an injected
// transport, executes them against a lcdtap::LcdTap instance and streams the
// JSON response back with bounded work per call. All state lives in a
// JsonIntf value, so several instances (e.g. USB CDC and HTTP) can run
// concurrently on the same LcdTap.
//
// Application-specific behaviour (host-side parameters, persistence, stats,
// extra commands) is injected through JsonIntfCallbacks.

#include <cstdint>

#include "lcdtap/lcdtap.hpp"
#include "lcdtap/pico2/json_lex.hpp"
#include "lcdtap/pico2/json_rle.hpp"

// =============================================================================
// Transport abstraction
// write() returning 0 means the TX buffer is full; the response generator
// retries the same data on the next flush (back-pressure).
// =============================================================================

struct JsonIntfTransport {
  void* ctx = nullptr;
  bool (*isConnected)(void* ctx) = nullptr;
  int (*getChar)(void* ctx) = nullptr;  // -1 = none available
  int (*write)(void* ctx, const char* data, int len) = nullptr;
};

// =============================================================================
// Parser
// =============================================================================

enum class JsonParseState {
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
  EXPECT_PRESET_COLON,
  EXPECT_PRESET_VALUE,
  EXPECT_WP_COLON,
  EXPECT_WP_VALUE,
  CMD_READY,
  ERROR,
};

// Compile-time bound on JsonIntfCallbacks::numHostParams; sizes the parser.
static constexpr int JSON_INTF_MAX_HOST_PARAMS = 4;

// A full setparams carries every config item plus the host-side settings, as
// the web UI echoes the whole set back rather than a diff. Deriving the limit
// from Configs means it cannot fall behind when settings are appended. The
// spare slots are headroom for clients that send extra keys.
static constexpr int JSON_INTF_MAX_PARAMS =
    static_cast<int>(lcdtap::Configs::NUM_CONFIGS) + JSON_INTF_MAX_HOST_PARAMS +
    6;

// Pool for string-valued parameters (e.g. setnetconfig SSID/PSK). Sized for a
// handful of strings of up to TOK_MAX_LEN chars each.
static constexpr int JSON_INTF_STR_POOL_SIZE = 384;

struct JsonParam {
  char key[TOK_MAX_LEN + 1];
  int32_t value;
  // String-valued parameter: value is 0 and strOfs points into
  // JsonParser::strPool. Integer consumers must skip these.
  bool isString;
  uint16_t strOfs;
};

struct JsonParser {
  JsonParseState state = JsonParseState::EXPECT_OBJ_OPEN;
  char command[TOK_MAX_LEN + 1] = {};
  JsonParam params[JSON_INTF_MAX_PARAMS] = {};
  int numParams = 0;
  // Set when more parameters arrive than params[] can hold. Reported as an
  // error rather than silently dropped: a silent drop makes the device answer
  // "ok" to a request it did not honour, which is very hard to diagnose from
  // the client side.
  bool paramOverflow = false;
  // Same philosophy for string values that do not fit the pool.
  bool strPoolOverflow = false;
  bool hasParams = false;
  char preset[TOK_MAX_LEN + 1] = {};
  bool writeProtected = false;
  char strPool[JSON_INTF_STR_POOL_SIZE] = {};
  int strPoolLen = 0;

  void reset() {
    state = JsonParseState::EXPECT_OBJ_OPEN;
    command[0] = '\0';
    numParams = 0;
    paramOverflow = false;
    strPoolOverflow = false;
    hasParams = false;
    preset[0] = '\0';
    writeProtected = false;
    strPoolLen = 0;
  }
};

// Returns the pool string of a string-valued parameter, or nullptr.
inline const char* jsonParamStr(const JsonParser& p, int i) {
  return p.params[i].isString ? p.strPool + p.params[i].strOfs : nullptr;
}

// =============================================================================
// Response generator
// =============================================================================

enum class JsonRespPhase {
  IDLE,
  SHORT,        // sending a fixed response string
  PARAMS,       // getparams: stream one param object at a time
  STATS,        // getstats: stream one stat entry at a time
  FB_HEADER,    // getframebuffer: initial JSON fragment
  FB_DATA,      // getframebuffer: base64 pixel stream
  FB_FOOTER,    // getframebuffer: closing fragment
  DUMP_HEADER,  // cmddump_read: initial JSON fragment
  DUMP_DATA,    // cmddump_read: base64 data stream
  DUMP_FOOTER,  // cmddump_read: closing fragment
};

struct JsonRespGen {
  JsonRespPhase phase = JsonRespPhase::IDLE;

  // SHORT / HEADER / FOOTER: pointer + remaining length into a static string
  const char* strPtr = nullptr;
  int strRem = 0;

  // PARAMS: which parameter index is being emitted
  int paramIdx = 0;

  // STATS: snapshot taken at command time and streaming position
  static constexpr int MAX_STATS = 16;
  lcdtap::StatEntry statsBuf[MAX_STATS] = {};
  int statsCount = 0;
  int statsIdx = 0;

  // FB_DATA / DUMP_DATA: pixel/byte position and base64 accumulator
  uint16_t fbOutX = 0;
  uint16_t fbOutY = 0;
  uint16_t fbPhysW = 0;
  uint16_t fbOutW = 0;
  uint16_t fbOutH = 0;
  uint16_t fbSrcX = 0;  // trim region origin X in physical buffer
  uint16_t fbSrcY = 0;  // trim region origin Y in physical buffer
  uint16_t fbSrcW = 0;  // trim region width  (pre-rotation)
  uint16_t fbSrcH = 0;  // trim region height (pre-rotation)
  uint8_t fbRot = 0;
  bool fbInverted = false;
  const uint16_t* fbPtr = nullptr;

  // FB_DATA: RLE segment pipeline. Pixels are gathered in output order into
  // rleSeg (one read per pixel, so a live framebuffer cannot desync the
  // stream), encoded into rlePkt, then drained through base64. rlePktPos is
  // the only resume point a TX-full retry needs.
  uint16_t rleSeg[RLE_SEG_MAX_PIXELS] = {};
  uint8_t rlePkt[RLE_SEG_MAX_BYTES] = {};
  int rlePktLen = 0;
  int rlePktPos = 0;

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

  // getframebuffer: whether write protection was engaged
  bool fbWriteProtected = false;

  // getparams: preset config (used when paramUsePreset is true)
  bool paramUsePreset = false;
  lcdtap::LcdTapConfig paramPresetCfg;

  // Per-call chunk buffer for formatted segments
  char chunkBuf[512] = {};
  int chunkLen = 0;
  int chunkPos = 0;
};

// =============================================================================
// Application callbacks
// Host-side parameters (settings that are not lcdtap Configs) and
// persistence are application policy, injected here so the engine has no
// dependency on any particular example.
// =============================================================================

struct JsonIntf;

struct JsonIntfCallbacks {
  void* ctx = nullptr;

  // Host params are interleaved into the getparams slot list immediately
  // before hostParamAnchorSlot, mirroring the OSD menu order.
  int numHostParams = 0;
  int hostParamAnchorSlot = 0;

  // Emit the JSON object body for host param hostIdx, starting with '{'. The
  // engine appends the "," / "]}" tail itself so the framing stays identical
  // across applications. Returns the would-be length, snprintf-style.
  int (*buildHostParamChunk)(int hostIdx, char* buf, int cap,
                             void* ctx) = nullptr;

  // setparams: called once before key staging begins.
  void (*beginSetParams)(void* ctx) = nullptr;
  // setparams: return true when the key was consumed as a host param.
  bool (*stageHostParam)(const char* key, int32_t value, void* ctx) = nullptr;
  // setparams: apply staged host params, switch the bus if needed and persist
  // everything, after the lcdtap config has been updated. Returns true when
  // the device must reboot for the change to take effect. When save is false
  // the client asked to apply without persisting; implementations should
  // still persist when they return true, or the reboot would revert the
  // change.
  bool (*commitParams)(const lcdtap::LcdTapConfig& cfg, lcdtap::BusType oldBus,
                       bool save, void* ctx) = nullptr;

  // Debug statistics providers.
  int (*statsCollect)(lcdtap::StatEntry* out, int maxCount,
                      void* ctx) = nullptr;
  void (*statsReset)(void* ctx) = nullptr;

  // Application-specific commands, tried after the built-ins and before the
  // "unknown command" fallback. Respond via jsonIntfRespondShort(). Return
  // true when the command was handled.
  bool (*execAppCommand)(JsonIntf* ji, const JsonParser& p,
                         void* ctx) = nullptr;
};

// =============================================================================
// Interface instance and public API
// =============================================================================

struct JsonIntf {
  lcdtap::LcdTap* inst = nullptr;
  const lcdtap::BusType* currentBus = nullptr;
  bool rebootPending = false;
  JsonIntfTransport trx;
  JsonIntfCallbacks cb;
  Lexer lex;
  JsonParser parser;
  JsonRespGen resp;
};

void jsonIntfInit(JsonIntf* ji, lcdtap::LcdTap* inst,
                  const lcdtap::BusType* currentBus,
                  const JsonIntfTransport& trx, const JsonIntfCallbacks& cb);

// Non-blocking pump: receive a bounded burst of characters, advance the
// parser, and flush pending response output. Call from the main loop.
void jsonIntfProcess(JsonIntf* ji);

// True when no response is queued, i.e. it is safe to reboot without cutting
// off the acknowledgement the client is waiting for.
bool jsonIntfRespIdle(const JsonIntf* ji);

// True once a command requested a reboot (see JsonIntfCallbacks
// commitParams and jsonIntfSetRebootPending).
bool jsonIntfRebootPending(const JsonIntf* ji);

// Drop any in-flight request/response and return to IDLE. Releases the write
// protection a getframebuffer may have engaged. For transports whose
// connection can die mid-stream (HTTP).
void jsonIntfAbort(JsonIntf* ji);

// Queue a complete single-line response. For execAppCommand implementations.
void jsonIntfRespondShort(JsonIntf* ji, const char* jsonLine);

// Request a reboot once the current response has drained. For
// execAppCommand implementations.
void jsonIntfSetRebootPending(JsonIntf* ji);
