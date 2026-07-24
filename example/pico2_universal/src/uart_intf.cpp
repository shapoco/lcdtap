#include "uart_intf.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "uart_b64.hpp"
#include "uart_lex.hpp"
#include "uart_trx.hpp"

// =============================================================================
// Module-level state
// =============================================================================

static lcdtap::LcdTap* gLcdTap = nullptr;
static lcdtap::BusType* gCurrentIface = nullptr;
static OutputInterface* gCurrentOutIf = nullptr;
static CompositeDacKind* gCurrentDac = nullptr;
static SwitchIfaceFn gSwitchIface = nullptr;
static SaveConfigFn gSaveConfig = nullptr;
static bool gRebootPending = false;

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

// A full setparams carries every cfgN plus the host-side settings, because the
// web UI echoes the whole set back rather than a diff. Deriving the limit from
// ConfigId means it cannot fall behind again the way a hardcoded 16 did once
// outputInterface and compositeDac were appended -- those two landed last on
// the wire and were exactly the ones dropped. The spare slots are headroom for
// clients that send extra keys.
// NUM_HOST_PARAMS lives in output_interface.hpp, next to the ordering anchor.
static constexpr int MAX_PARAMS =
    static_cast<int>(lcdtap::ConfigId::NUM_CONFIGS) + NUM_HOST_PARAMS + 6;

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
  uint16_t fbOutW = 0;
  uint16_t fbOutH = 0;
  uint16_t fbSrcX = 0;  // trim region origin X in physical buffer
  uint16_t fbSrcY = 0;  // trim region origin Y in physical buffer
  uint16_t fbSrcW = 0;  // trim region width  (pre-rotation)
  uint16_t fbSrcH = 0;  // trim region height (pre-rotation)
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

static RespGen gResp;

// Attempt to drain gResp.chunkBuf. Returns true when fully drained.
static bool drainChunk() {
  while (gResp.chunkPos < gResp.chunkLen) {
    int rem = gResp.chunkLen - gResp.chunkPos;
    int sent = trxWrite(gResp.chunkBuf + gResp.chunkPos, rem);
    if (sent == 0) return false;  // TX buffer full — retry next call
    gResp.chunkPos += sent;
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

// Total entries in the getparams list, and the slot the host-side settings
// occupy. The OSD inserts them immediately before HOST_PARAM_ANCHOR, so
// using the same anchor here keeps the two menus in the same order.
static constexpr int NUM_PARAM_SLOTS =
    static_cast<int>(lcdtap::ConfigId::NUM_CONFIGS) + NUM_HOST_PARAMS;
static constexpr int HOST_PARAM_SLOT = static_cast<int>(HOST_PARAM_ANCHOR);

// Emission slot -> ConfigId. Only meaningful for slots that are not host
// settings. Note that slot and ConfigId diverge past HOST_PARAM_SLOT: the
// "cfgN" number MUST come from here and never from the slot, or setparams
// would write the value into a different setting.
static lcdtap::ConfigId configIdForSlot(int slot) {
  return static_cast<lcdtap::ConfigId>(
      slot < HOST_PARAM_SLOT ? slot : slot - NUM_HOST_PARAMS);
}

static bool slotIsHostParam(int slot) {
  return slot >= HOST_PARAM_SLOT && slot < HOST_PARAM_SLOT + NUM_HOST_PARAMS;
}

// Build the JSON fragment for one emission slot into chunkBuf.
// Returns false when slot is out of range.
static bool buildParamChunk(int slot, const lcdtap::LcdTapConfig& cfg,
                            lcdtap::BusType iface, OutputInterface outIf,
                            CompositeDacKind dac) {
  if (slot < 0 || slot >= NUM_PARAM_SLOTS) return false;

  // Every item ends the same way; only the last one closes the array.
  const char* tail = (slot == NUM_PARAM_SLOTS - 1) ? "}]}\r\n" : "},";

  char* buf = gResp.chunkBuf;
  int cap = static_cast<int>(sizeof(gResp.chunkBuf));
  int pos = 0;

  if (slot == 0) {
    pos += snprintf(buf + pos, static_cast<size_t>(cap - pos), "{\"params\":[");
  }

  if (slotIsHostParam(slot)) {
    if (slot == HOST_PARAM_SLOT) {
      // Composite output needs GPIOs the parallel bus already owns.
      pos += snprintf(
          buf + pos, static_cast<size_t>(cap - pos),
          "{\"id\":\"outputInterface\",\"type\":\"ENUM\","
          "\"name\":\"Output Interface\",\"unit\":null,"
          "\"options\":{\"DVI-D\":0,\"NTSC\":1,\"PAL\":2},\"value\":%d,"
          "\"enableKeyId\":\"cfg%d\",\"enableKeyValueMin\":0,"
          "\"enableKeyValueMax\":%d%s",
          static_cast<int>(outIf),
          static_cast<int>(lcdtap::ConfigId::BUS_INTERFACE),
          static_cast<int>(lcdtap::BusType::PARALLEL) - 1, tail);
    } else {
      // Gated on the output interface, mirroring the OSD: the DAC only means
      // anything once a composite mode is selected. A client that honours the
      // enable-key cascade also greys this out when outputInterface itself is
      // disabled, which is how the parallel bus rules it out.
      pos += snprintf(buf + pos, static_cast<size_t>(cap - pos),
                      "{\"id\":\"compositeDac\",\"type\":\"ENUM\","
                      "\"name\":\"Video DAC Type\",\"unit\":null,"
                      "\"options\":{\"PWM\":0,\"R-2R\":1},\"value\":%d,"
                      "\"enableKeyId\":\"outputInterface\","
                      "\"enableKeyValueMin\":%d,\"enableKeyValueMax\":%d%s",
                      static_cast<int>(dac),
                      static_cast<int>(OutputInterface::NTSC),
                      static_cast<int>(OutputInterface::PAL), tail);
    }
    gResp.chunkLen = pos < cap ? pos : cap - 1;
    gResp.chunkPos = 0;
    return true;
  }

  lcdtap::ConfigEntry e;
  lcdtap::ConfigId cfgId = configIdForSlot(slot);
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

  gResp.chunkLen = pos < cap ? pos : cap - 1;
  gResp.chunkPos = 0;
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
    respSetShort(presetResp);
    return;
  }

  // ----- getparams -----
  if (strcmp(cmd, "getparams") == 0) {
    gResp.phase = RespPhase::PARAMS;
    gResp.paramIdx = 0;
    gResp.chunkLen = 0;
    gResp.chunkPos = 0;
    if (p.preset[0] != '\0') {
      gResp.paramUsePreset = makePresetConfig(p.preset, &gResp.paramPresetCfg);
    } else {
      gResp.paramUsePreset = false;
    }
    return;
  }

  // ----- setparams -----
  if (strcmp(cmd, "setparams") == 0) {
    // Applying a truncated request would silently honour some settings and
    // drop others while still reporting success.
    if (p.paramOverflow) {
      respSetShort("{\"error\":\"too many params\"}\r\n");
      return;
    }
    lcdtap::LcdTapConfig cfg = gLcdTap->getConfig();
    lcdtap::BusType oldIface = *gCurrentIface;
    OutputInterface newOutIf = *gCurrentOutIf;
    CompositeDacKind newDac = *gCurrentDac;

    for (int i = 0; i < p.numParams; i++) {
      const char* k = p.params[i].key;
      int32_t v = p.params[i].value;
      // Host-side settings; not ConfigIds, so they have their own keys.
      if (strcmp(k, "outputInterface") == 0) {
        if (v >= 0 && v < static_cast<int32_t>(OUTPUT_INTERFACE_COUNT)) {
          newOutIf = static_cast<OutputInterface>(v);
        }
        continue;
      }
      if (strcmp(k, "compositeDac") == 0) {
        if (v >= 0 && v < static_cast<int32_t>(COMPOSITE_DAC_KIND_COUNT)) {
          newDac = static_cast<CompositeDacKind>(v);
        }
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

    lcdtap::Status st = gLcdTap->updateConfig(cfg);
    if (st != lcdtap::Status::OK) {
      respSetShort("{\"error\":\"updateConfig failed\"}\r\n");
      return;
    }

    // Composite is unavailable on the parallel bus and the R-2R ladder is
    // unavailable on I2C, so a combined change must not persist an illegal
    // pair. Order matters: the DAC is clamped against the clamped bus.
    newOutIf = outputInterfaceSanitize(newOutIf, cfg.busInterface);
    newDac = compositeDacSanitize(newDac, cfg.busInterface);

    const bool busChanged = (cfg.busInterface != oldIface);
    const bool outIfChanged = (newOutIf != *gCurrentOutIf);
    const bool dacChanged = (newDac != *gCurrentDac);
    // A bus or DAC change while composite is running also needs a reboot: the
    // DAC binds to its pins and peripheral at init, and the R-2R ladder
    // occupies GPIO5-11, overlapping the I2C pins. Switching in place would
    // hand GPIO8/9 to the I2C driver while the PIO still drives them.
    const bool needReboot =
        outIfChanged ||
        (outputInterfaceIsComposite(newOutIf) && (busChanged || dacChanged));

    if (busChanged && !needReboot) {
      gSwitchIface(cfg.busInterface);
    }
    // Keep the live values in step when no reboot will do it for us, so a
    // getparams straight after Apply reports what was actually applied.
    if (!needReboot) {
      *gCurrentOutIf = newOutIf;
      *gCurrentDac = newDac;
    }

    ConfigFile toSave = {};
    toSave.libConfig = gLcdTap->getConfig();
    toSave.outputInterface = static_cast<uint8_t>(newOutIf);
    toSave.compositeDac = static_cast<uint8_t>(newDac);
    gSaveConfig(toSave);

    respSetShort("{\"response\":\"ok\"}\r\n");
    // Reboot only after the response has been flushed, so the client sees it.
    if (needReboot) gRebootPending = true;
    return;
  }

  // ----- getframebuffer -----
  if (strcmp(cmd, "getframebuffer") == 0) {
    gResp.fbWriteProtected = p.writeProtected;
    if (p.writeProtected) gLcdTap->setWriteProtected(true);

    lcdtap::LcdTapConfig cfg = gLcdTap->getConfig();
    uint16_t physW = cfg.buffWidth;
    uint16_t physH = cfg.buffHeight;

    uint16_t srcX, srcY, srcW, srcH;
    gLcdTap->getOutSrcRegion(&srcX, &srcY, &srcW, &srcH);
    if (srcW == 0 || srcH == 0) {
      srcX = 0;
      srcY = 0;
      srcW = physW;
      srcH = physH;
    }

    uint8_t rot = cfg.outputRotation & 3u;

    gResp.fbPhysW = physW;
    gResp.fbSrcX = srcX;
    gResp.fbSrcY = srcY;
    gResp.fbSrcW = srcW;
    gResp.fbSrcH = srcH;
    gResp.fbRot = rot;
    gResp.fbInverted = gLcdTap->isOutputInverted();
    gResp.fbPtr = gLcdTap->getFramebuf();
    if ((rot & 1u) == 0u) {
      gResp.fbOutW = srcW;
      gResp.fbOutH = srcH;
    } else {
      gResp.fbOutW = srcH;
      gResp.fbOutH = srcW;
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
    int sent = trxWrite(gResp.b64Out + gResp.b64OutPos, 4 - gResp.b64OutPos);
    if (sent == 0) return false;
    gResp.b64OutPos += sent;
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
      lcdtap::LcdTapConfig cfg;
      lcdtap::BusType iface;
      if (gResp.paramUsePreset) {
        cfg = gResp.paramPresetCfg;
        iface = gResp.paramPresetCfg.busInterface;
      } else {
        cfg = gLcdTap->getConfig();
        iface = *gCurrentIface;
      }
      // outputInterface is a host setting, so presets never carry one; always
      // report the live value.
      if (!buildParamChunk(gResp.paramIdx, cfg, iface, *gCurrentOutIf,
                           *gCurrentDac)) {
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
        if (gResp.fbWriteProtected) gLcdTap->setWriteProtected(false);
        break;
      }

      // Process pixels until CDC is full or end of row.
      // Advance fbOutX/fbOutY BEFORE b64Feed so a CDC-full retry does not
      // re-feed the same pixel a second time.
      while (gResp.fbOutY < gResp.fbOutH) {
        if (!b64DrainOut()) break;  // drain before reading next pixel

        uint32_t idx = fbIndexTrimmed(gResp.fbOutX, gResp.fbOutY, gResp.fbSrcX,
                                      gResp.fbSrcY, gResp.fbSrcW, gResp.fbSrcH,
                                      gResp.fbPhysW, gResp.fbRot);
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

void uartIfInit(lcdtap::LcdTap* lcdtap, lcdtap::BusType* currentIface,
                OutputInterface* currentOutIf, CompositeDacKind* currentDac,
                SwitchIfaceFn switchIface, SaveConfigFn saveConfig) {
  gLcdTap = lcdtap;
  gCurrentIface = currentIface;
  gCurrentOutIf = currentOutIf;
  gCurrentDac = currentDac;
  gSwitchIface = switchIface;
  gSaveConfig = saveConfig;
  gRebootPending = false;

  trxInit();

  gLex.reset();
  gParser.reset();
  gResp = {};
}

bool uartIfRebootPending() { return gRebootPending; }

bool uartIfRespIdle() { return gResp.phase == RespPhase::IDLE; }

void uartIfProcess() {
  if (!trxIsConnected()) return;

  // Receive up to a small burst of characters per call to avoid starving
  // other main-loop work.
  for (int i = 0; i < 64; i++) {
    int c = trxGetChar();
    if (c < 0) break;
    processRxChar(static_cast<char>(c));
  }

  respFlush();
}
