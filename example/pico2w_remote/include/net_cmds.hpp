#pragma once

// Application-specific JSON commands for pico2w_remote:
//   getnetconfig / setnetconfig / netstatus
// Wired into the JSON engine via JsonIntfCallbacks::execAppCommand.

#include "lcdtap/pico2/json_intf.hpp"

#include "net_config.hpp"

// Persist hook: park Core 1, save, resume. Provided by main.cpp.
using NetConfigSaveFn = void (*)(const NetConfig& cfg);

void netCmdsInit(NetConfig* liveCfg, NetConfigSaveFn saveFn);

// JsonIntfCallbacks::execAppCommand implementation.
bool netCmdsExec(JsonIntf* ji, const JsonParser& p, void* ctx);
