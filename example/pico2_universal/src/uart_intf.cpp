#include "uart_intf.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lcdtap/pico2/json_b64.hpp"
#include "lcdtap/pico2/json_lex.hpp"
#include "lcdtap/pico2/json_rle.hpp"
#include "stats.hpp"
#include "uart_trx.hpp"

// =============================================================================
// Transport abstraction
// Injected at init time so the same engine can run over USB CDC or a TCP
// connection. write() returning 0 means the TX buffer is full; the response
// generator retries the same data on the next flush (back-pressure).
// =============================================================================

struct JsonIntfTransport {
  void* ctx = nullptr;
  bool (*isConnected)(void* ctx) = nullptr;
  int (*getChar)(void* ctx) = nullptr;  // -1 = none available
  int (*write)(void* ctx, const char* data, int len) = nullptr;
};

// =============================================================================
// Application callbacks
// Host-side parameters (settings that are not lcdtap ConfigIds) and
// persistence are application policy, injected here so the engine has no
// dependency on any particular example.
// =============================================================================

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
  // the device must reboot for the change to take effect.
  bool (*commitParams)(const lcdtap::LcdTapConfig& cfg, lcdtap::BusType oldBus,
                       void* ctx) = nullptr;

  // Debug statistics providers.
  int (*statsCollect)(lcdtap::StatEntry* out, int maxCount,
                      void* ctx) = nullptr;
  void (*statsReset)(void* ctx) = nullptr;
};

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
  EXPECT_PRESET_COLON,
  EXPECT_PRESET_VALUE,
  EXPECT_WP_COLON,
  EXPECT_WP_VALUE,
  CMD_READY,
  ERROR,
};

// Compile-time bound on JsonIntfCallbacks::numHostParams; sizes the parser.
static constexpr int JSON_INTF_MAX_HOST_PARAMS = 4;

// A full setparams carries every cfgN plus the host-side settings, because the
// web UI echoes the whole set back rather than a diff. Deriving the limit from
// ConfigId means it cannot fall behind again the way a hardcoded 16 did once
// outputInterface and compositeDac were appended -- those two landed last on
// the wire and were exactly the ones dropped. The spare slots are headroom for
// clients that send extra keys.
static constexpr int MAX_PARAMS =
    static_cast<int>(lcdtap::ConfigId::NUM_CONFIGS) +
    JSON_INTF_MAX_HOST_PARAMS + 6;

struct Param {
  char key[TOK_MAX_LEN + 1];
  int32_t value;
};

struct Parser {
  ParseState state = ParseState::EXPECT_OBJ_OPEN;
  char command[TOK_MAX_LEN + 1] = {};
  Param params[MAX_PARAMS] = {};
  int numParams = 0;
  // Set when more parameters arrive than params[] can hold. Reported as an
  // error rather than silently dropped: a silent drop makes the device answer
  // "ok" to a request it did not honour, which is very hard to diagnose from
  // the client side.
  bool paramOverflow = false;
  bool hasParams = false;
  char preset[TOK_MAX_LEN + 1] = {};
  bool writeProtected = false;

  void reset() {
    state = ParseState::EXPECT_OBJ_OPEN;
    command[0] = '\0';
    numParams = 0;
    paramOverflow = false;
    hasParams = false;
    preset[0] = '\0';
    writeProtected = false;
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
        } else if (strcmp(tok.buf, "preset") == 0) {
          p.state = ParseState::EXPECT_PRESET_COLON;
        } else if (strcmp(tok.buf, "writeProtected") == 0) {
          p.state = ParseState::EXPECT_WP_COLON;
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
        } else {
          p.paramOverflow = true;
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

    case ParseState::EXPECT_PRESET_COLON:
      if (tok.type == TokType::COLON) {
        p.state = ParseState::EXPECT_PRESET_VALUE;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_PRESET_VALUE:
      if (tok.type == TokType::STRING) {
        strncpy(p.preset, tok.buf, TOK_MAX_LEN);
        p.preset[TOK_MAX_LEN] = '\0';
        p.state = ParseState::EXPECT_AFTER_TOP_KV;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_WP_COLON:
      if (tok.type == TokType::COLON) {
        p.state = ParseState::EXPECT_WP_VALUE;
      } else {
        p.state = ParseState::ERROR;
        return true;
      }
      break;

    case ParseState::EXPECT_WP_VALUE:
      if (tok.type == TokType::BOOL_TRUE) {
        p.writeProtected = true;
        p.state = ParseState::EXPECT_AFTER_TOP_KV;
      } else if (tok.type == TokType::BOOL_FALSE) {
        p.writeProtected = false;
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
  STATS,        // getstats: stream one stat entry at a time
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
  // the only resume point a CDC-full retry needs.
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
// Interface instance
// All former file-scope state lives here so several instances (USB CDC, HTTP)
// can run concurrently.
// =============================================================================

struct JsonIntf {
  lcdtap::LcdTap* inst = nullptr;
  lcdtap::BusType* currentIface = nullptr;
  bool rebootPending = false;
  JsonIntfTransport trx;
  JsonIntfCallbacks cb;
  Lexer lex;
  Parser parser;
  RespGen resp;
};

static JsonIntf gIf;

// Attempt to drain ji.resp.chunkBuf. Returns true when fully drained.
static bool drainChunk(JsonIntf& ji) {
  RespGen& r = ji.resp;
  while (r.chunkPos < r.chunkLen) {
    int rem = r.chunkLen - r.chunkPos;
    int sent = ji.trx.write(ji.trx.ctx, r.chunkBuf + r.chunkPos, rem);
    if (sent == 0) return false;  // TX buffer full — retry next call
    r.chunkPos += sent;
  }
  r.chunkLen = 0;
  r.chunkPos = 0;
  return true;
}

// Load a string into chunkBuf.
static void chunkFromStr(JsonIntf& ji, const char* s) {
  RespGen& r = ji.resp;
  int len = static_cast<int>(strlen(s));
  if (len > static_cast<int>(sizeof(r.chunkBuf) - 1))
    len = static_cast<int>(sizeof(r.chunkBuf) - 1);
  memcpy(r.chunkBuf, s, static_cast<size_t>(len));
  r.chunkLen = len;
  r.chunkPos = 0;
}

// =============================================================================
// getparams output helpers
// =============================================================================

// Total entries in the getparams list. The host-side settings occupy the
// slots immediately before the application's anchor slot, mirroring how the
// OSD inserts them, so the two menus stay in the same order.
static int numParamSlots(const JsonIntf& ji) {
  return static_cast<int>(lcdtap::ConfigId::NUM_CONFIGS) + ji.cb.numHostParams;
}

// Emission slot -> ConfigId. Only meaningful for slots that are not host
// settings. Note that slot and ConfigId diverge past the anchor slot: the
// "cfgN" number MUST come from here and never from the slot, or setparams
// would write the value into a different setting.
static lcdtap::ConfigId configIdForSlot(const JsonIntf& ji, int slot) {
  return static_cast<lcdtap::ConfigId>(
      slot < ji.cb.hostParamAnchorSlot ? slot : slot - ji.cb.numHostParams);
}

static bool slotIsHostParam(const JsonIntf& ji, int slot) {
  return slot >= ji.cb.hostParamAnchorSlot &&
         slot < ji.cb.hostParamAnchorSlot + ji.cb.numHostParams;
}

// Build the JSON fragment for one emission slot into chunkBuf.
// Returns false when slot is out of range.
static bool buildParamChunk(JsonIntf& ji, int slot,
                            const lcdtap::LcdTapConfig& cfg,
                            lcdtap::BusType iface) {
  if (slot < 0 || slot >= numParamSlots(ji)) return false;

  // Every item ends the same way; only the last one closes the array.
  const char* tail = (slot == numParamSlots(ji) - 1) ? "}]}\r\n" : "},";

  char* buf = ji.resp.chunkBuf;
  int cap = static_cast<int>(sizeof(ji.resp.chunkBuf));
  int pos = 0;

  if (slot == 0) {
    pos += snprintf(buf + pos, static_cast<size_t>(cap - pos), "{\"params\":[");
  }

  if (slotIsHostParam(ji, slot)) {
    pos += ji.cb.buildHostParamChunk(slot - ji.cb.hostParamAnchorSlot,
                                     buf + pos, cap - pos, ji.cb.ctx);
    pos += snprintf(buf + pos, static_cast<size_t>(cap - pos), "%s", tail);
    ji.resp.chunkLen = pos < cap ? pos : cap - 1;
    ji.resp.chunkPos = 0;
    return true;
  }

  lcdtap::ConfigEntry e;
  lcdtap::ConfigId cfgId = configIdForSlot(ji, slot);
  lcdtap::getConfigEntryById(cfgId, &e);

  int16_t value;
  if (cfgId == lcdtap::ConfigId::BUS_INTERFACE) {
    value = static_cast<int16_t>(iface);
  } else {
    value = lcdtap::getConfigValueById(cfg, cfgId);
  }

  const char* typeStr = (e.type == lcdtap::ValueType::INT16)  ? "INTEGER"
                        : (e.type == lcdtap::ValueType::BOOL) ? "BOOLEAN"
                                                              : "ENUM";
  pos += snprintf(buf + pos, static_cast<size_t>(cap - pos),
                  "{\"id\":\"cfg%d\",\"type\":\"%s\",\"name\":\"%s\",",
                  static_cast<int>(cfgId), typeStr, e.name);

  if (e.unit && e.unit[0] != '\0') {
    pos += snprintf(buf + pos, static_cast<size_t>(cap - pos),
                    "\"unit\":\"%s\",", e.unit);
  } else {
    pos +=
        snprintf(buf + pos, static_cast<size_t>(cap - pos), "\"unit\":null,");
  }

  if (e.type == lcdtap::ValueType::INT16) {
    pos +=
        snprintf(buf + pos, static_cast<size_t>(cap - pos),
                 "\"min\":%d,\"max\":%d,\"step\":%d,", static_cast<int>(e.min),
                 static_cast<int>(e.max), static_cast<int>(e.step));
  } else if (e.type == lcdtap::ValueType::ENUM) {
    pos += snprintf(buf + pos, static_cast<size_t>(cap - pos), "\"options\":{");
    for (int i = 0; i <= e.max - e.min; ++i) {
      if (i > 0) {
        pos += snprintf(buf + pos, static_cast<size_t>(cap - pos), ",");
      }
      pos += snprintf(buf + pos, static_cast<size_t>(cap - pos), "\"%s\":%d",
                      e.options[i], static_cast<int>(e.min) + i);
    }
    pos += snprintf(buf + pos, static_cast<size_t>(cap - pos), "},");
  }

  if (e.type == lcdtap::ValueType::BOOL) {
    pos += snprintf(buf + pos, static_cast<size_t>(cap - pos), "\"value\":%s",
                    value ? "true" : "false");
  } else {
    pos += snprintf(buf + pos, static_cast<size_t>(cap - pos), "\"value\":%d",
                    static_cast<int>(value));
  }

  if (e.enableKeyId >= 0) {
    pos += snprintf(buf + pos, static_cast<size_t>(cap - pos),
                    ",\"enableKeyId\":\"cfg%d\",\"enableKeyValueMin\":%d"
                    ",\"enableKeyValueMax\":%d",
                    static_cast<int>(e.enableKeyId),
                    static_cast<int>(e.enableKeyValueMin),
                    static_cast<int>(e.enableKeyValueMax));
  }

  pos += snprintf(buf + pos, static_cast<size_t>(cap - pos), "%s", tail);

  ji.resp.chunkLen = pos < cap ? pos : cap - 1;
  ji.resp.chunkPos = 0;
  return true;
}

// =============================================================================
// getstats output helpers
// =============================================================================

static const char* statFmtStr(lcdtap::StatFormat fmt) {
  switch (fmt) {
    case lcdtap::StatFormat::HEX: return "hex";
    case lcdtap::StatFormat::RATE: return "rate";
    default: return "dec";
  }
}

// Build the JSON fragment for one stats entry into chunkBuf.
// Returns false when idx is out of range.
static bool buildStatChunk(JsonIntf& ji, int idx) {
  if (idx < 0 || idx >= ji.resp.statsCount) return false;

  const lcdtap::StatEntry& e = ji.resp.statsBuf[idx];
  const char* tail = (idx == ji.resp.statsCount - 1) ? "}]}\r\n" : "},";

  char* buf = ji.resp.chunkBuf;
  int cap = static_cast<int>(sizeof(ji.resp.chunkBuf));
  int pos = 0;

  if (idx == 0) {
    pos += snprintf(buf + pos, static_cast<size_t>(cap - pos), "{\"stats\":[");
  }
  pos += snprintf(buf + pos, static_cast<size_t>(cap - pos),
                  "{\"name\":\"%s\",\"value\":%lu,\"unit\":\"%s\","
                  "\"fmt\":\"%s\"%s",
                  e.name, static_cast<unsigned long>(e.value),
                  e.unit ? e.unit : "", statFmtStr(e.fmt), tail);

  ji.resp.chunkLen = pos < cap ? pos : cap - 1;
  ji.resp.chunkPos = 0;
  return true;
}

// =============================================================================
// Preset config helper
// =============================================================================

static bool makePresetConfig(const char* name, lcdtap::LcdTapConfig* cfgOut) {
  for (int i = 0; i < static_cast<int>(lcdtap::ConfigPreset::NUM_PRESETS);
       ++i) {
    if (strcmp(name, lcdtap::CONFIG_PRESET_NAMES[i]) == 0) {
      lcdtap::getPresetConfig(static_cast<lcdtap::ConfigPreset>(i), cfgOut);
      return true;
    }
  }
  return false;
}

// =============================================================================
// Command execution
// =============================================================================

static void respSetShort(JsonIntf& ji, const char* s) {
  ji.resp.phase = RespPhase::SHORT;
  chunkFromStr(ji, s);
}

static void execCommand(JsonIntf& ji, const Parser& p) {
  const char* cmd = p.command;
  RespGen& r = ji.resp;

  // ----- hello -----
  if (strcmp(cmd, "hello") == 0) {
    respSetShort(ji, "{\"response\":\"welcome lcdtap\"}\r\n");
    return;
  }

  // ----- getpresets -----
  if (strcmp(cmd, "getpresets") == 0) {
    char presetResp[256];
    int pos = snprintf(presetResp, sizeof(presetResp), "{\"presets\":[");
    for (int i = 0; i < static_cast<int>(lcdtap::ConfigPreset::NUM_PRESETS);
         ++i) {
      if (i > 0)
        pos += snprintf(presetResp + pos,
                        sizeof(presetResp) - static_cast<size_t>(pos), ",");
      pos += snprintf(presetResp + pos,
                      sizeof(presetResp) - static_cast<size_t>(pos), "\"%s\"",
                      lcdtap::CONFIG_PRESET_NAMES[i]);
    }
    snprintf(presetResp + pos, sizeof(presetResp) - static_cast<size_t>(pos),
             "]}\r\n");
    respSetShort(ji, presetResp);
    return;
  }

  // ----- getparams -----
  if (strcmp(cmd, "getparams") == 0) {
    r.phase = RespPhase::PARAMS;
    r.paramIdx = 0;
    r.chunkLen = 0;
    r.chunkPos = 0;
    if (p.preset[0] != '\0') {
      r.paramUsePreset = makePresetConfig(p.preset, &r.paramPresetCfg);
    } else {
      r.paramUsePreset = false;
    }
    return;
  }

  // ----- setparams -----
  if (strcmp(cmd, "setparams") == 0) {
    // Applying a truncated request would silently honour some settings and
    // drop others while still reporting success.
    if (p.paramOverflow) {
      respSetShort(ji, "{\"error\":\"too many params\"}\r\n");
      return;
    }
    lcdtap::LcdTapConfig cfg = ji.inst->getConfig();
    lcdtap::BusType oldIface = *ji.currentIface;
    if (ji.cb.beginSetParams) ji.cb.beginSetParams(ji.cb.ctx);

    for (int i = 0; i < p.numParams; i++) {
      const char* k = p.params[i].key;
      int32_t v = p.params[i].value;
      // Host-side settings; not ConfigIds, so they have their own keys.
      if (ji.cb.stageHostParam && ji.cb.stageHostParam(k, v, ji.cb.ctx)) {
        continue;
      }
      // Accept "cfgN" keys and map to ConfigId by index.
      if (k[0] == 'c' && k[1] == 'f' && k[2] == 'g' && k[3] != '\0') {
        long idx = strtol(k + 3, nullptr, 10);
        if (idx >= 0 &&
            idx < static_cast<long>(lcdtap::ConfigId::NUM_CONFIGS)) {
          lcdtap::setConfigValueById(&cfg, static_cast<lcdtap::ConfigId>(idx),
                                     static_cast<int16_t>(v));
        }
      }
    }

    lcdtap::Status st = ji.inst->updateConfig(cfg);
    if (st != lcdtap::Status::OK) {
      respSetShort(ji, "{\"error\":\"updateConfig failed\"}\r\n");
      return;
    }

    // Sanitizing staged host params, switching the bus and persisting are
    // application policy.
    const bool needReboot = ji.cb.commitParams
                                ? ji.cb.commitParams(cfg, oldIface, ji.cb.ctx)
                                : false;

    respSetShort(ji, "{\"response\":\"ok\"}\r\n");
    // Reboot only after the response has been flushed, so the client sees it.
    if (needReboot) ji.rebootPending = true;
    return;
  }

  // ----- getstats -----
  if (strcmp(cmd, "getstats") == 0) {
    r.statsCount =
        ji.cb.statsCollect
            ? ji.cb.statsCollect(r.statsBuf, RespGen::MAX_STATS, ji.cb.ctx)
            : 0;
    r.statsIdx = 0;
    if (r.statsCount == 0) {
      respSetShort(ji, "{\"stats\":[]}\r\n");
      return;
    }
    r.chunkLen = 0;
    r.chunkPos = 0;
    r.phase = RespPhase::STATS;
    return;
  }

  // ----- statsreset -----
  if (strcmp(cmd, "statsreset") == 0) {
    if (ji.cb.statsReset) ji.cb.statsReset(ji.cb.ctx);
    respSetShort(ji, "{\"response\":\"ok\"}\r\n");
    return;
  }

  // ----- getframebuffer -----
  if (strcmp(cmd, "getframebuffer") == 0) {
    r.fbWriteProtected = p.writeProtected;
    if (p.writeProtected) ji.inst->setWriteProtected(true);

    lcdtap::LcdTapConfig cfg = ji.inst->getConfig();
    uint16_t physW = cfg.buffWidth;
    uint16_t physH = cfg.buffHeight;

    uint16_t srcX, srcY, srcW, srcH;
    ji.inst->getOutSrcRegion(&srcX, &srcY, &srcW, &srcH);
    if (srcW == 0 || srcH == 0) {
      srcX = 0;
      srcY = 0;
      srcW = physW;
      srcH = physH;
    }

    uint8_t rot = cfg.outputRotation & 3u;

    r.fbPhysW = physW;
    r.fbSrcX = srcX;
    r.fbSrcY = srcY;
    r.fbSrcW = srcW;
    r.fbSrcH = srcH;
    r.fbRot = rot;
    r.fbInverted = ji.inst->isOutputInverted();
    r.fbPtr = ji.inst->getFramebuf();
    if ((rot & 1u) == 0u) {
      r.fbOutW = srcW;
      r.fbOutH = srcH;
    } else {
      r.fbOutW = srcH;
      r.fbOutH = srcW;
    }
    r.fbOutX = 0;
    r.fbOutY = 0;
    r.rlePktLen = 0;
    r.rlePktPos = 0;
    r.b64Count = 0;
    r.b64OutPos = 4;  // force next chunk

    // Build header string into chunkBuf
    snprintf(
        r.chunkBuf, sizeof(r.chunkBuf),
        "{\"width\":%d,\"height\":%d,\"format\":\"RGB565-RLE\",\"data\":\"",
        static_cast<int>(r.fbOutW), static_cast<int>(r.fbOutH));
    r.chunkLen = static_cast<int>(strlen(r.chunkBuf));
    r.chunkPos = 0;
    r.phase = RespPhase::FB_HEADER;
    return;
  }

  // ----- cmddump_start -----
  if (strcmp(cmd, "cmddump_start") == 0) {
    ji.inst->dumpStart(lcdtap::getDefaultDumpConfig());
    respSetShort(ji, "{\"response\":\"ok\"}\r\n");
    return;
  }

  // ----- cmddump_abort -----
  if (strcmp(cmd, "cmddump_abort") == 0) {
    ji.inst->dumpAbort();
    respSetShort(ji, "{\"response\":\"ok\"}\r\n");
    return;
  }

  // ----- cmddump_forcetrigger -----
  if (strcmp(cmd, "cmddump_forcetrigger") == 0) {
    ji.inst->dumpForceTrigger();
    respSetShort(ji, "{\"response\":\"ok\"}\r\n");
    return;
  }

  // ----- cmddump_getstatus -----
  if (strcmp(cmd, "cmddump_getstatus") == 0) {
    const char* stStr = "WAIT";
    switch (ji.inst->dumpGetState()) {
      case lcdtap::DumpState::WAIT: stStr = "WAIT"; break;
      case lcdtap::DumpState::ACTIVE: stStr = "ACTIVE"; break;
      case lcdtap::DumpState::COMPLETE: stStr = "COMPLETE"; break;
    }
    uint16_t dumpSize = ji.inst->dumpGetSize();
    snprintf(r.chunkBuf, sizeof(r.chunkBuf),
             "{\"status\":\"%s\",\"bytes\":%d}\r\n", stStr,
             static_cast<int>(dumpSize) * 2);
    r.chunkLen = static_cast<int>(strlen(r.chunkBuf));
    r.chunkPos = 0;
    r.phase = RespPhase::SHORT;
    return;
  }

  // ----- cmddump_read -----
  if (strcmp(cmd, "cmddump_read") == 0) {
    uint16_t dlen = ji.inst->dumpGetSize();
    r.dumpPtr = ji.inst->dumpGetBuffer();
    r.dumpLen = dlen;
    r.dumpPos = 0;
    r.dumpHighByte = false;
    r.b64Count = 0;
    r.b64OutPos = 4;

    snprintf(r.chunkBuf, sizeof(r.chunkBuf), "{\"length\":%d,\"data\":\"",
             static_cast<int>(dlen));
    r.chunkLen = static_cast<int>(strlen(r.chunkBuf));
    r.chunkPos = 0;
    r.phase = RespPhase::DUMP_HEADER;
    return;
  }

  // ----- unknown -----
  respSetShort(ji, "{\"error\":\"unknown command\"}\r\n");
}

// =============================================================================
// Response flushing
// =============================================================================

// Map output pixel coordinate (dx, dy) to frameBuffer index within the trim
// region (srcX, srcY, srcW, srcH), then to physical buffer index.
static inline uint32_t fbIndexTrimmed(uint16_t dx, uint16_t dy, uint16_t srcX,
                                      uint16_t srcY, uint16_t srcW,
                                      uint16_t srcH, uint16_t physW,
                                      uint8_t rot) {
  uint32_t bx, by;
  switch (rot) {
    default:
    case 0:
      bx = srcX + dx;
      by = srcY + dy;
      break;
    case 1:
      bx = srcX + dy;
      by = srcY + srcH - 1u - dx;
      break;
    case 2:
      bx = srcX + srcW - 1u - dx;
      by = srcY + srcH - 1u - dy;
      break;
    case 3:
      bx = srcX + srcW - 1u - dy;
      by = srcY + dx;
      break;
  }
  return by * physW + bx;
}

// Feed one byte into the base64 accumulator; if a full 3-byte block is ready
// encode it and store 4 chars into b64Out.
static bool b64Feed(JsonIntf& ji, uint8_t byte) {
  RespGen& r = ji.resp;
  r.b64Acc[r.b64Count++] = byte;
  if (r.b64Count == 3) {
    b64Encode3(r.b64Acc, r.b64Out);
    r.b64Count = 0;
    r.b64OutPos = 0;
    return true;
  }
  return false;
}

// Flush pending base64 output chars. Returns true when flushed (or nothing
// pending).
static bool b64DrainOut(JsonIntf& ji) {
  RespGen& r = ji.resp;
  while (r.b64OutPos < 4) {
    int sent =
        ji.trx.write(ji.trx.ctx, r.b64Out + r.b64OutPos, 4 - r.b64OutPos);
    if (sent == 0) return false;
    r.b64OutPos += sent;
  }
  return true;
}

// Flush the base64 padding at stream end. Returns true when done.
static bool b64FlushPad(JsonIntf& ji) {
  RespGen& r = ji.resp;
  if (r.b64Count == 0) return true;
  b64EncodePad(r.b64Acc, r.b64Count, r.b64Out);
  r.b64Count = 0;
  r.b64OutPos = 0;
  return b64DrainOut(ji);
}

// Advance the response generator state machine; call repeatedly until IDLE.
static void respFlush(JsonIntf& ji) {
  RespGen& r = ji.resp;
  switch (r.phase) {
    case RespPhase::IDLE: break;

    case RespPhase::SHORT:
      if (drainChunk(ji)) r.phase = RespPhase::IDLE;
      break;

    case RespPhase::PARAMS: {
      if (!drainChunk(ji)) break;
      lcdtap::LcdTapConfig cfg;
      lcdtap::BusType iface;
      if (r.paramUsePreset) {
        cfg = r.paramPresetCfg;
        iface = r.paramPresetCfg.busInterface;
      } else {
        cfg = ji.inst->getConfig();
        iface = *ji.currentIface;
      }
      // outputInterface is a host setting, so presets never carry one; always
      // report the live value.
      if (!buildParamChunk(ji, r.paramIdx, cfg, iface)) {
        r.phase = RespPhase::IDLE;
        break;
      }
      r.paramIdx++;
      // drainChunk will be called on the next respFlush() invocation
      break;
    }

    case RespPhase::STATS: {
      if (!drainChunk(ji)) break;
      if (!buildStatChunk(ji, r.statsIdx)) {
        r.phase = RespPhase::IDLE;
        break;
      }
      r.statsIdx++;
      break;
    }

    case RespPhase::FB_HEADER:
      if (drainChunk(ji)) r.phase = RespPhase::FB_DATA;
      break;

    case RespPhase::FB_DATA: {
      // First drain any pending base64 output chars
      if (!b64DrainOut(ji)) break;

      // Drain the encoded packet bytes of the current segment. Advance
      // rlePktPos BEFORE checking the drain result so a CDC-full retry does
      // not re-feed the same byte (it already sits in b64Out).
      while (r.rlePktPos < r.rlePktLen) {
        if (b64Feed(ji, r.rlePkt[r.rlePktPos++]) && !b64DrainOut(ji)) break;
      }
      if (r.rlePktPos < r.rlePktLen || !b64DrainOut(ji)) break;

      if (r.fbPtr == nullptr || r.fbOutY >= r.fbOutH) {
        // All pixels done — flush padding
        if (!b64FlushPad(ji)) break;
        chunkFromStr(ji, "\"}\r\n");
        r.phase = RespPhase::FB_FOOTER;
        if (r.fbWriteProtected) ji.inst->setWriteProtected(false);
        break;
      }

      // Gather the next segment (bounded by the row end so runs never cross
      // rows), then encode it. Each pixel is read exactly once, so a live
      // framebuffer can tear the image but never desync the packet stream.
      int n = r.fbOutW - r.fbOutX;
      if (n > RLE_SEG_MAX_PIXELS) n = RLE_SEG_MAX_PIXELS;
      for (int i = 0; i < n; i++) {
        uint32_t idx = fbIndexTrimmed(static_cast<uint16_t>(r.fbOutX + i),
                                      r.fbOutY, r.fbSrcX, r.fbSrcY, r.fbSrcW,
                                      r.fbSrcH, r.fbPhysW, r.fbRot);
        uint16_t px = r.fbPtr[idx];
        if (r.fbInverted) px ^= 0xFFFFu;
        r.rleSeg[i] = px;
      }
      r.fbOutX = static_cast<uint16_t>(r.fbOutX + n);
      if (r.fbOutX >= r.fbOutW) {
        r.fbOutX = 0;
        r.fbOutY++;
      }
      r.rlePktLen = rleEncodeSegment(r.rleSeg, n, r.rlePkt);
      r.rlePktPos = 0;
      // Yield once per segment (<= 128 px) to keep IRQ latency low.
      break;
    }

    case RespPhase::FB_FOOTER:
      if (drainChunk(ji)) r.phase = RespPhase::IDLE;
      break;

    case RespPhase::DUMP_HEADER:
      if (drainChunk(ji)) r.phase = RespPhase::DUMP_DATA;
      break;

    case RespPhase::DUMP_DATA: {
      if (!b64DrainOut(ji)) break;

      if (r.dumpPos >= r.dumpLen) {
        if (!b64FlushPad(ji)) break;
        chunkFromStr(ji, "\"}\r\n");
        r.phase = RespPhase::DUMP_FOOTER;
        break;
      }

      // One uint16_t = 2 bytes (little-endian).
      // Advance dumpPos BEFORE b64Feed so a CDC-full retry does not re-feed
      // the same word.
      // Per word at most one b64Feed returns true (low XOR high, never both),
      // so draining after low is safe and we must always feed high regardless.
      uint16_t word = r.dumpPtr[r.dumpPos++];
      if (b64Feed(ji, static_cast<uint8_t>(word & 0xFFu))) b64DrainOut(ji);
      if (b64Feed(ji, static_cast<uint8_t>(word >> 8))) b64DrainOut(ji);
      break;
    }

    case RespPhase::DUMP_FOOTER:
      if (drainChunk(ji)) r.phase = RespPhase::IDLE;
      break;
  }
}

// =============================================================================
// RX processing
// =============================================================================

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

static void processRxChar(JsonIntf& ji, char c) {
  bool tokReady = lexPush(ji.lex, c);

  if (tokReady) {
    bool done = parserFeed(ji.parser, ji.lex.pending);
    if (done) {
      if (ji.parser.state == ParseState::CMD_READY) {
        execCommand(ji, ji.parser);
      } else {
        snprintf(ji.resp.chunkBuf, sizeof(ji.resp.chunkBuf),
                 "{\"error\":\"parse error\",\"state\":\"%s\"}\r\n",
                 parseStateStr(ji.parser.state));
        ji.resp.chunkLen = static_cast<int>(strlen(ji.resp.chunkBuf));
        ji.resp.chunkPos = 0;
        ji.resp.phase = RespPhase::SHORT;
      }
      ji.parser.reset();
      ji.lex.reset();
    }
  }

  // If CRLF arrived but no token, treat as end-of-command attempt
  if (ji.lex.lineReady) {
    ji.lex.lineReady = false;
    if (ji.parser.state != ParseState::EXPECT_OBJ_OPEN) {
      // Incomplete command — reset quietly
      ji.parser.reset();
      ji.lex.reset();
    }
  }

  // Process delimiter deferred from end of a literal token
  if (ji.lex.deferredChar) {
    char dc = ji.lex.deferredChar;
    ji.lex.deferredChar = 0;
    processRxChar(ji, dc);
  }
}

static void jsonIntfProcess(JsonIntf& ji) {
  if (!ji.trx.isConnected(ji.trx.ctx)) return;

  // Receive up to a small burst of characters per call to avoid starving
  // other main-loop work.
  for (int i = 0; i < 64; i++) {
    int c = ji.trx.getChar(ji.trx.ctx);
    if (c < 0) break;
    processRxChar(ji, static_cast<char>(c));
  }

  respFlush(ji);
}

// =============================================================================
// USB CDC transport adapter
// =============================================================================

static bool cdcIsConnected(void* /*ctx*/) { return trxIsConnected(); }

static int cdcGetChar(void* /*ctx*/) { return trxGetChar(); }

static int cdcWrite(void* /*ctx*/, const char* data, int len) {
  return trxWrite(data, len);
}

// =============================================================================
// Universal-specific host params (outputInterface, compositeDac), bus
// switching and flash persistence, injected via JsonIntfCallbacks.
// =============================================================================

struct UniversalHostCtx {
  lcdtap::LcdTap* inst = nullptr;
  OutputInterface* currentOutIf = nullptr;
  CompositeDacKind* currentDac = nullptr;
  SwitchIfaceFn switchIface = nullptr;
  SaveConfigFn saveConfig = nullptr;
  // setparams staging, seeded from the live values by beginSetParams.
  OutputInterface stagedOutIf = OutputInterface::DVI_D;
  CompositeDacKind stagedDac = CompositeDacKind::PWM;
};

static UniversalHostCtx gHostCtx;

static_assert(NUM_HOST_PARAMS <= JSON_INTF_MAX_HOST_PARAMS,
              "grow JSON_INTF_MAX_HOST_PARAMS");

static int uniBuildHostParamChunk(int hostIdx, char* buf, int cap, void* ctx) {
  UniversalHostCtx& hc = *static_cast<UniversalHostCtx*>(ctx);
  if (hostIdx == 0) {
    // Composite and DisplayLink need GPIOs the parallel bus already owns.
    return snprintf(
        buf, static_cast<size_t>(cap),
        "{\"id\":\"outputInterface\",\"type\":\"ENUM\","
        "\"name\":\"Output Interface\",\"unit\":null,"
        "\"options\":{\"DVI-D\":0,\"NTSC\":1,\"PAL\":2,\"DisplayLink\":3},"
        "\"value\":%d,"
        "\"enableKeyId\":\"cfg%d\",\"enableKeyValueMin\":0,"
        "\"enableKeyValueMax\":%d",
        static_cast<int>(*hc.currentOutIf),
        static_cast<int>(lcdtap::ConfigId::BUS_INTERFACE),
        static_cast<int>(lcdtap::BusType::PARALLEL) - 1);
  }
  // Gated on the output interface, mirroring the OSD: the DAC only means
  // anything once a composite mode is selected. A client that honours the
  // enable-key cascade also greys this out when outputInterface itself is
  // disabled, which is how the parallel bus rules it out.
  return snprintf(buf, static_cast<size_t>(cap),
                  "{\"id\":\"compositeDac\",\"type\":\"ENUM\","
                  "\"name\":\"Video DAC Type\",\"unit\":null,"
                  "\"options\":{\"PWM\":0,\"R-2R\":1},\"value\":%d,"
                  "\"enableKeyId\":\"outputInterface\","
                  "\"enableKeyValueMin\":%d,\"enableKeyValueMax\":%d",
                  static_cast<int>(*hc.currentDac),
                  static_cast<int>(OutputInterface::NTSC),
                  static_cast<int>(OutputInterface::PAL));
}

static void uniBeginSetParams(void* ctx) {
  UniversalHostCtx& hc = *static_cast<UniversalHostCtx*>(ctx);
  hc.stagedOutIf = *hc.currentOutIf;
  hc.stagedDac = *hc.currentDac;
}

static bool uniStageHostParam(const char* key, int32_t value, void* ctx) {
  UniversalHostCtx& hc = *static_cast<UniversalHostCtx*>(ctx);
  if (strcmp(key, "outputInterface") == 0) {
    if (value >= 0 && value < static_cast<int32_t>(OUTPUT_INTERFACE_COUNT)) {
      hc.stagedOutIf = static_cast<OutputInterface>(value);
    }
    return true;
  }
  if (strcmp(key, "compositeDac") == 0) {
    if (value >= 0 && value < static_cast<int32_t>(COMPOSITE_DAC_KIND_COUNT)) {
      hc.stagedDac = static_cast<CompositeDacKind>(value);
    }
    return true;
  }
  return false;
}

static bool uniCommitParams(const lcdtap::LcdTapConfig& cfg,
                            lcdtap::BusType oldBus, void* ctx) {
  UniversalHostCtx& hc = *static_cast<UniversalHostCtx*>(ctx);

  // Composite is unavailable on the parallel bus and the R-2R ladder is
  // unavailable on I2C, so a combined change must not persist an illegal
  // pair. Order matters: the DAC is clamped against the clamped bus.
  OutputInterface newOutIf =
      outputInterfaceSanitize(hc.stagedOutIf, cfg.busInterface);
  CompositeDacKind newDac =
      compositeDacSanitize(hc.stagedDac, cfg.busInterface);

  const bool busChanged = (cfg.busInterface != oldBus);
  const bool outIfChanged = (newOutIf != *hc.currentOutIf);
  const bool dacChanged = (newDac != *hc.currentDac);
  // A bus or DAC change while composite is running also needs a reboot: the
  // DAC binds to its pins and peripheral at init, and the R-2R ladder
  // occupies GPIO5-11, overlapping the I2C pins. Switching in place would
  // hand GPIO8/9 to the I2C driver while the PIO still drives them.
  const bool needReboot =
      outIfChanged ||
      (outputInterfaceIsComposite(newOutIf) && (busChanged || dacChanged));

  if (busChanged && !needReboot) {
    hc.switchIface(cfg.busInterface);
  }
  // Keep the live values in step when no reboot will do it for us, so a
  // getparams straight after Apply reports what was actually applied.
  if (!needReboot) {
    *hc.currentOutIf = newOutIf;
    *hc.currentDac = newDac;
  }

  ConfigFile toSave = {};
  toSave.libConfig = hc.inst->getConfig();
  toSave.outputInterface = static_cast<uint8_t>(newOutIf);
  toSave.compositeDac = static_cast<uint8_t>(newDac);
  hc.saveConfig(toSave);

  return needReboot;
}

static int uniStatsCollect(lcdtap::StatEntry* out, int maxCount,
                           void* /*ctx*/) {
  return statsCollect(out, maxCount);
}

static void uniStatsReset(void* /*ctx*/) { statsReset(); }

// =============================================================================
// Public API
// =============================================================================

void uartIfInit(lcdtap::LcdTap* lcdtap, lcdtap::BusType* currentIface,
                OutputInterface* currentOutIf, CompositeDacKind* currentDac,
                SwitchIfaceFn switchIface, SaveConfigFn saveConfig) {
  gHostCtx.inst = lcdtap;
  gHostCtx.currentOutIf = currentOutIf;
  gHostCtx.currentDac = currentDac;
  gHostCtx.switchIface = switchIface;
  gHostCtx.saveConfig = saveConfig;

  gIf.inst = lcdtap;
  gIf.currentIface = currentIface;
  gIf.rebootPending = false;
  gIf.trx = {nullptr, cdcIsConnected, cdcGetChar, cdcWrite};
  gIf.cb = {};
  gIf.cb.ctx = &gHostCtx;
  gIf.cb.numHostParams = NUM_HOST_PARAMS;
  gIf.cb.hostParamAnchorSlot = static_cast<int>(HOST_PARAM_ANCHOR);
  gIf.cb.buildHostParamChunk = uniBuildHostParamChunk;
  gIf.cb.beginSetParams = uniBeginSetParams;
  gIf.cb.stageHostParam = uniStageHostParam;
  gIf.cb.commitParams = uniCommitParams;
  gIf.cb.statsCollect = uniStatsCollect;
  gIf.cb.statsReset = uniStatsReset;

  trxInit();

  gIf.lex.reset();
  gIf.parser.reset();
  gIf.resp = {};
}

bool uartIfRebootPending() { return gIf.rebootPending; }

bool uartIfRespIdle() { return gIf.resp.phase == RespPhase::IDLE; }

void uartIfProcess() { jsonIntfProcess(gIf); }
