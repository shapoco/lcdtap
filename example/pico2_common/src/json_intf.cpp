#include "lcdtap/pico2/json_intf.hpp"

#include <cstdio>
#include <cstring>

#include "lcdtap/pico2/json_b64.hpp"

// =============================================================================
// Parser
// =============================================================================

// Feed one token to the parser. Returns true when CMD_READY or ERROR.
static bool parserFeed(JsonParser& p, const Token& tok) {
  switch (p.state) {
    case JsonParseState::EXPECT_OBJ_OPEN:
      if (tok.type == TokType::BRACE_OPEN) {
        p.state = JsonParseState::EXPECT_TOP_KEY_OR_CLOSE;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_TOP_KEY_OR_CLOSE:
      if (tok.type == TokType::BRACE_CLOSE) {
        p.state = JsonParseState::CMD_READY;
        return true;
      } else if (tok.type == TokType::STRING) {
        if (strcmp(tok.buf, "command") == 0) {
          p.state = JsonParseState::EXPECT_CMD_COLON;
        } else if (strcmp(tok.buf, "params") == 0) {
          p.state = JsonParseState::EXPECT_PARAMS_COLON;
        } else if (strcmp(tok.buf, "preset") == 0) {
          p.state = JsonParseState::EXPECT_PRESET_COLON;
        } else if (strcmp(tok.buf, "writeProtected") == 0) {
          p.state = JsonParseState::EXPECT_WP_COLON;
        } else {
          p.state = JsonParseState::ERROR;
          return true;
        }
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_CMD_COLON:
      if (tok.type == TokType::COLON) {
        p.state = JsonParseState::EXPECT_CMD_VALUE;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_CMD_VALUE:
      if (tok.type == TokType::STRING) {
        strncpy(p.command, tok.buf, TOK_MAX_LEN);
        p.command[TOK_MAX_LEN] = '\0';
        p.state = JsonParseState::EXPECT_AFTER_TOP_KV;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_PARAMS_COLON:
      if (tok.type == TokType::COLON) {
        p.state = JsonParseState::EXPECT_PARAMS_OBJ_OPEN;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_PARAMS_OBJ_OPEN:
      if (tok.type == TokType::BRACE_OPEN) {
        p.hasParams = true;
        p.state = JsonParseState::EXPECT_PARAM_KEY_OR_CLOSE;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_PARAM_KEY_OR_CLOSE:
      if (tok.type == TokType::BRACE_CLOSE) {
        p.state = JsonParseState::EXPECT_AFTER_TOP_KV;
      } else if (tok.type == TokType::STRING) {
        if (p.numParams < JSON_INTF_MAX_PARAMS) {
          strncpy(p.params[p.numParams].key, tok.buf, TOK_MAX_LEN);
          p.params[p.numParams].key[TOK_MAX_LEN] = '\0';
        } else {
          p.paramOverflow = true;
        }
        p.state = JsonParseState::EXPECT_PARAM_COLON;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_PARAM_COLON:
      if (tok.type == TokType::COLON) {
        p.state = JsonParseState::EXPECT_PARAM_VALUE;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_PARAM_VALUE:
      if (tok.type == TokType::INTEGER) {
        if (p.numParams < JSON_INTF_MAX_PARAMS) {
          p.params[p.numParams].value = tok.intVal;
          p.params[p.numParams].isString = false;
          p.numParams++;
        }
        p.state = JsonParseState::EXPECT_AFTER_PARAM;
      } else if (tok.type == TokType::BOOL_TRUE) {
        if (p.numParams < JSON_INTF_MAX_PARAMS) {
          p.params[p.numParams].value = 1;
          p.params[p.numParams].isString = false;
          p.numParams++;
        }
        p.state = JsonParseState::EXPECT_AFTER_PARAM;
      } else if (tok.type == TokType::BOOL_FALSE) {
        if (p.numParams < JSON_INTF_MAX_PARAMS) {
          p.params[p.numParams].value = 0;
          p.params[p.numParams].isString = false;
          p.numParams++;
        }
        p.state = JsonParseState::EXPECT_AFTER_PARAM;
      } else if (tok.type == TokType::STRING) {
        if (p.numParams < JSON_INTF_MAX_PARAMS) {
          int len = static_cast<int>(strlen(tok.buf));
          if (p.strPoolLen + len + 1 <= JSON_INTF_STR_POOL_SIZE) {
            p.params[p.numParams].value = 0;
            p.params[p.numParams].isString = true;
            p.params[p.numParams].strOfs = static_cast<uint16_t>(p.strPoolLen);
            memcpy(p.strPool + p.strPoolLen, tok.buf,
                   static_cast<size_t>(len + 1));
            p.strPoolLen += len + 1;
            p.numParams++;
          } else {
            p.strPoolOverflow = true;
          }
        }
        p.state = JsonParseState::EXPECT_AFTER_PARAM;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_AFTER_PARAM:
      if (tok.type == TokType::COMMA) {
        p.state = JsonParseState::EXPECT_PARAM_KEY_OR_CLOSE;
      } else if (tok.type == TokType::BRACE_CLOSE) {
        p.state = JsonParseState::EXPECT_AFTER_TOP_KV;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_PRESET_COLON:
      if (tok.type == TokType::COLON) {
        p.state = JsonParseState::EXPECT_PRESET_VALUE;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_PRESET_VALUE:
      if (tok.type == TokType::STRING) {
        strncpy(p.preset, tok.buf, TOK_MAX_LEN);
        p.preset[TOK_MAX_LEN] = '\0';
        p.state = JsonParseState::EXPECT_AFTER_TOP_KV;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_WP_COLON:
      if (tok.type == TokType::COLON) {
        p.state = JsonParseState::EXPECT_WP_VALUE;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_WP_VALUE:
      if (tok.type == TokType::BOOL_TRUE) {
        p.writeProtected = true;
        p.state = JsonParseState::EXPECT_AFTER_TOP_KV;
      } else if (tok.type == TokType::BOOL_FALSE) {
        p.writeProtected = false;
        p.state = JsonParseState::EXPECT_AFTER_TOP_KV;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::EXPECT_AFTER_TOP_KV:
      if (tok.type == TokType::COMMA) {
        p.state = JsonParseState::EXPECT_TOP_KEY_OR_CLOSE;
      } else if (tok.type == TokType::BRACE_CLOSE) {
        p.state = JsonParseState::CMD_READY;
        return true;
      } else {
        p.state = JsonParseState::ERROR;
        return true;
      }
      break;

    case JsonParseState::CMD_READY:
    case JsonParseState::ERROR: break;
  }
  return p.state == JsonParseState::CMD_READY ||
         p.state == JsonParseState::ERROR;
}

// =============================================================================
// Chunk buffer helpers
// =============================================================================

// Attempt to drain ji.resp.chunkBuf. Returns true when fully drained.
static bool drainChunk(JsonIntf& ji) {
  JsonRespGen& r = ji.resp;
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
  JsonRespGen& r = ji.resp;
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
  return static_cast<int>(lcdtap::Configs::NUM_CONFIGS) + ji.cb.numHostParams;
}

// Emission slot -> Configs. Only meaningful for slots that are not host
// settings. Note that slot and Configs diverge past the anchor slot: the
// CONFIG_IDS key MUST come from here and never from the slot, or setparams
// would write the value into a different setting.
static lcdtap::Configs configForSlot(const JsonIntf& ji, int slot) {
  return static_cast<lcdtap::Configs>(
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
  lcdtap::Configs config = configForSlot(ji, slot);
  lcdtap::getConfigEntryById(config, &e);

  int16_t value;
  if (config == lcdtap::Configs::BUS_INTERFACE) {
    value = static_cast<int16_t>(iface);
  } else {
    value = lcdtap::getConfigValueById(cfg, config);
  }

  const char* typeStr = (e.type == lcdtap::ValueType::INT16)  ? "INTEGER"
                        : (e.type == lcdtap::ValueType::HEX)  ? "HEX"
                        : (e.type == lcdtap::ValueType::BOOL) ? "BOOLEAN"
                                                              : "ENUM";
  pos +=
      snprintf(buf + pos, static_cast<size_t>(cap - pos),
               "{\"id\":\"%s\",\"type\":\"%s\",\"name\":\"%s\",",
               lcdtap::CONFIG_IDS[static_cast<int>(config)], typeStr, e.name);

  if (e.unit && e.unit[0] != '\0') {
    pos += snprintf(buf + pos, static_cast<size_t>(cap - pos),
                    "\"unit\":\"%s\",", e.unit);
  } else {
    pos +=
        snprintf(buf + pos, static_cast<size_t>(cap - pos), "\"unit\":null,");
  }

  if (e.type == lcdtap::ValueType::INT16 || e.type == lcdtap::ValueType::HEX) {
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
                    ",\"enableKeyId\":\"%s\",\"enableKeyValueMin\":%d"
                    ",\"enableKeyValueMax\":%d",
                    lcdtap::CONFIG_IDS[e.enableKeyId],
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
  ji.resp.phase = JsonRespPhase::SHORT;
  chunkFromStr(ji, s);
}

static void execCommand(JsonIntf& ji, const JsonParser& p) {
  const char* cmd = p.command;
  JsonRespGen& r = ji.resp;

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
    r.phase = JsonRespPhase::PARAMS;
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
    if (p.paramOverflow || p.strPoolOverflow) {
      respSetShort(ji, "{\"error\":\"too many params\"}\r\n");
      return;
    }
    lcdtap::LcdTapConfig cfg = ji.inst->getConfig();
    lcdtap::BusType oldIface = *ji.currentBus;
    if (ji.cb.beginSetParams) ji.cb.beginSetParams(ji.cb.ctx);

    bool save = true;
    for (int i = 0; i < p.numParams; i++) {
      const char* k = p.params[i].key;
      int32_t v = p.params[i].value;
      // All setparams values are numeric; string values only appear in
      // app-specific commands.
      if (p.params[i].isString) continue;
      // Protocol-level option, not a config item: "save": false applies the
      // settings without persisting them (avoids flash wear for automated
      // callers that reconfigure the device continuously).
      if (strcmp(k, "save") == 0) {
        save = (v != 0);
        continue;
      }
      // Host-side settings; not Configs, so they have their own keys.
      if (ji.cb.stageHostParam && ji.cb.stageHostParam(k, v, ji.cb.ctx)) {
        continue;
      }
      // Config item keys are the CONFIG_IDS identifiers; unknown keys are
      // silently ignored so exports from other examples import cleanly.
      lcdtap::Configs c = lcdtap::findConfigByKey(k);
      if (c != lcdtap::Configs::NUM_CONFIGS) {
        lcdtap::setConfigValueById(&cfg, c, static_cast<int16_t>(v));
      }
    }

    lcdtap::Status st = ji.inst->updateConfig(cfg);
    if (st != lcdtap::Status::OK) {
      respSetShort(ji, "{\"error\":\"updateConfig failed\"}\r\n");
      return;
    }

    // Sanitizing staged host params, switching the bus and persisting are
    // application policy.
    const bool needReboot =
        ji.cb.commitParams ? ji.cb.commitParams(cfg, oldIface, save, ji.cb.ctx)
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
            ? ji.cb.statsCollect(r.statsBuf, JsonRespGen::MAX_STATS, ji.cb.ctx)
            : 0;
    r.statsIdx = 0;
    if (r.statsCount == 0) {
      respSetShort(ji, "{\"stats\":[]}\r\n");
      return;
    }
    r.chunkLen = 0;
    r.chunkPos = 0;
    r.phase = JsonRespPhase::STATS;
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
    r.phase = JsonRespPhase::FB_HEADER;
    return;
  }

  // ----- gettextbuffer -----
  if (strcmp(cmd, "gettextbuffer") == 0) {
    uint16_t cols, rows;
    ji.inst->getTextBufferSize(&cols, &rows);

    // 40x4 is the largest layout normalizeConfig() allows; the whole
    // response fits chunkBuf, so no streaming phase is needed. The snapshot
    // is taken in one call, so the text cannot tear.
    uint8_t text[160];
    uint32_t len = 0;
    if (cols > 0 && rows > 0) {
      uint32_t total = static_cast<uint32_t>(cols) * rows;
      if (total > sizeof(text)) total = sizeof(text);
      len = ji.inst->readTextBuffer(0, total, text);
    }

    char b64[(sizeof(text) + 2) / 3 * 4 + 1];
    int b64Len = 0;
    uint32_t consumed = 0;
    for (; consumed + 3 <= len; consumed += 3) {
      b64Encode3(text + consumed, b64 + b64Len);
      b64Len += 4;
    }
    if (consumed < len) {
      b64EncodePad(text + consumed, static_cast<int>(len - consumed),
                   b64 + b64Len);
      b64Len += 4;
    }
    b64[b64Len] = '\0';

    snprintf(r.chunkBuf, sizeof(r.chunkBuf),
             "{\"cols\":%d,\"rows\":%d,\"data\":\"%s\"}\r\n",
             static_cast<int>(cols), static_cast<int>(rows), b64);
    r.chunkLen = static_cast<int>(strlen(r.chunkBuf));
    r.chunkPos = 0;
    r.phase = JsonRespPhase::SHORT;
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
    r.phase = JsonRespPhase::SHORT;
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
    r.phase = JsonRespPhase::DUMP_HEADER;
    return;
  }

  // ----- application-specific commands -----
  if (ji.cb.execAppCommand && ji.cb.execAppCommand(&ji, p, ji.cb.ctx)) {
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
  JsonRespGen& r = ji.resp;
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
  JsonRespGen& r = ji.resp;
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
  JsonRespGen& r = ji.resp;
  if (r.b64Count == 0) return true;
  b64EncodePad(r.b64Acc, r.b64Count, r.b64Out);
  r.b64Count = 0;
  r.b64OutPos = 0;
  return b64DrainOut(ji);
}

// Advance the response generator state machine; call repeatedly until IDLE.
static void respFlush(JsonIntf& ji) {
  JsonRespGen& r = ji.resp;
  switch (r.phase) {
    case JsonRespPhase::IDLE: break;

    case JsonRespPhase::SHORT:
      if (drainChunk(ji)) r.phase = JsonRespPhase::IDLE;
      break;

    case JsonRespPhase::PARAMS: {
      if (!drainChunk(ji)) break;
      lcdtap::LcdTapConfig cfg;
      lcdtap::BusType iface;
      if (r.paramUsePreset) {
        cfg = r.paramPresetCfg;
        iface = r.paramPresetCfg.busInterface;
      } else {
        cfg = ji.inst->getConfig();
        iface = *ji.currentBus;
      }
      // Host params are host settings, so presets never carry one; the
      // application callback always reports the live values.
      if (!buildParamChunk(ji, r.paramIdx, cfg, iface)) {
        r.phase = JsonRespPhase::IDLE;
        break;
      }
      r.paramIdx++;
      // drainChunk will be called on the next respFlush() invocation
      break;
    }

    case JsonRespPhase::STATS: {
      if (!drainChunk(ji)) break;
      if (!buildStatChunk(ji, r.statsIdx)) {
        r.phase = JsonRespPhase::IDLE;
        break;
      }
      r.statsIdx++;
      break;
    }

    case JsonRespPhase::FB_HEADER:
      if (drainChunk(ji)) r.phase = JsonRespPhase::FB_DATA;
      break;

    case JsonRespPhase::FB_DATA: {
      // First drain any pending base64 output chars
      if (!b64DrainOut(ji)) break;

      // Drain the encoded packet bytes of the current segment. Advance
      // rlePktPos BEFORE checking the drain result so a TX-full retry does
      // not re-feed the same byte (it already sits in b64Out).
      while (r.rlePktPos < r.rlePktLen) {
        if (b64Feed(ji, r.rlePkt[r.rlePktPos++]) && !b64DrainOut(ji)) break;
      }
      if (r.rlePktPos < r.rlePktLen || !b64DrainOut(ji)) break;

      if (r.fbPtr == nullptr || r.fbOutY >= r.fbOutH) {
        // All pixels done — flush padding
        if (!b64FlushPad(ji)) break;
        chunkFromStr(ji, "\"}\r\n");
        r.phase = JsonRespPhase::FB_FOOTER;
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

    case JsonRespPhase::FB_FOOTER:
      if (drainChunk(ji)) r.phase = JsonRespPhase::IDLE;
      break;

    case JsonRespPhase::DUMP_HEADER:
      if (drainChunk(ji)) r.phase = JsonRespPhase::DUMP_DATA;
      break;

    case JsonRespPhase::DUMP_DATA: {
      if (!b64DrainOut(ji)) break;

      if (r.dumpPos >= r.dumpLen) {
        if (!b64FlushPad(ji)) break;
        chunkFromStr(ji, "\"}\r\n");
        r.phase = JsonRespPhase::DUMP_FOOTER;
        break;
      }

      // One uint16_t = 2 bytes (little-endian).
      // Advance dumpPos BEFORE b64Feed so a TX-full retry does not re-feed
      // the same word.
      // Per word at most one b64Feed returns true (low XOR high, never both),
      // so draining after low is safe and we must always feed high regardless.
      uint16_t word = r.dumpPtr[r.dumpPos++];
      if (b64Feed(ji, static_cast<uint8_t>(word & 0xFFu))) b64DrainOut(ji);
      if (b64Feed(ji, static_cast<uint8_t>(word >> 8))) b64DrainOut(ji);
      break;
    }

    case JsonRespPhase::DUMP_FOOTER:
      if (drainChunk(ji)) r.phase = JsonRespPhase::IDLE;
      break;
  }
}

// =============================================================================
// RX processing
// =============================================================================

static const char* parseStateStr(JsonParseState s) {
  switch (s) {
    case JsonParseState::EXPECT_OBJ_OPEN: return "EXPECT_OBJ_OPEN";
    case JsonParseState::EXPECT_TOP_KEY_OR_CLOSE: return "EXPECT_TOP_KEY";
    case JsonParseState::EXPECT_CMD_COLON: return "EXPECT_CMD_COLON";
    case JsonParseState::EXPECT_CMD_VALUE: return "EXPECT_CMD_VALUE";
    case JsonParseState::EXPECT_PARAMS_COLON: return "EXPECT_PARAMS_COLON";
    case JsonParseState::EXPECT_PARAMS_OBJ_OPEN: return "EXPECT_PARAMS_OBJ";
    case JsonParseState::EXPECT_PARAM_KEY_OR_CLOSE: return "EXPECT_PARAM_KEY";
    case JsonParseState::EXPECT_PARAM_COLON: return "EXPECT_PARAM_COLON";
    case JsonParseState::EXPECT_PARAM_VALUE: return "EXPECT_PARAM_VALUE";
    case JsonParseState::EXPECT_AFTER_PARAM: return "EXPECT_AFTER_PARAM";
    case JsonParseState::EXPECT_AFTER_TOP_KV: return "EXPECT_AFTER_TOP_KV";
    default: return "UNKNOWN";
  }
}

static void processRxChar(JsonIntf& ji, char c) {
  bool tokReady = lexPush(ji.lex, c);

  if (tokReady) {
    bool done = parserFeed(ji.parser, ji.lex.pending);
    if (done) {
      if (ji.parser.state == JsonParseState::CMD_READY) {
        execCommand(ji, ji.parser);
      } else {
        snprintf(ji.resp.chunkBuf, sizeof(ji.resp.chunkBuf),
                 "{\"error\":\"parse error\",\"state\":\"%s\"}\r\n",
                 parseStateStr(ji.parser.state));
        ji.resp.chunkLen = static_cast<int>(strlen(ji.resp.chunkBuf));
        ji.resp.chunkPos = 0;
        ji.resp.phase = JsonRespPhase::SHORT;
      }
      ji.parser.reset();
      ji.lex.reset();
    }
  }

  // If CRLF arrived but no token, treat as end-of-command attempt
  if (ji.lex.lineReady) {
    ji.lex.lineReady = false;
    if (ji.parser.state != JsonParseState::EXPECT_OBJ_OPEN) {
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

// =============================================================================
// Public API
// =============================================================================

void jsonIntfInit(JsonIntf* ji, lcdtap::LcdTap* inst,
                  const lcdtap::BusType* currentBus,
                  const JsonIntfTransport& trx, const JsonIntfCallbacks& cb) {
  ji->inst = inst;
  ji->currentBus = currentBus;
  ji->rebootPending = false;
  ji->trx = trx;
  ji->cb = cb;
  ji->lex.reset();
  ji->parser.reset();
  ji->resp = {};
}

void jsonIntfProcess(JsonIntf* ji) {
  if (!ji->trx.isConnected(ji->trx.ctx)) return;

  // Receive up to a small burst of characters per call to avoid starving
  // other main-loop work.
  for (int i = 0; i < 64; i++) {
    int c = ji->trx.getChar(ji->trx.ctx);
    if (c < 0) break;
    processRxChar(*ji, static_cast<char>(c));
  }

  respFlush(*ji);
}

bool jsonIntfRespIdle(const JsonIntf* ji) {
  return ji->resp.phase == JsonRespPhase::IDLE;
}

bool jsonIntfRebootPending(const JsonIntf* ji) { return ji->rebootPending; }

void jsonIntfAbort(JsonIntf* ji) {
  // A getframebuffer releases write protection only on its success path;
  // an aborted transfer must not leave it engaged.
  if (ji->resp.fbWriteProtected &&
      (ji->resp.phase == JsonRespPhase::FB_HEADER ||
       ji->resp.phase == JsonRespPhase::FB_DATA)) {
    ji->inst->setWriteProtected(false);
  }
  ji->lex.reset();
  ji->parser.reset();
  ji->resp = {};
}

void jsonIntfRespondShort(JsonIntf* ji, const char* jsonLine) {
  respSetShort(*ji, jsonLine);
}

void jsonIntfSetRebootPending(JsonIntf* ji) { ji->rebootPending = true; }
