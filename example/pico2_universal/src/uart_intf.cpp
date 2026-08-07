#include "uart_intf.hpp"

#include <cstdio>
#include <cstring>

#include "lcdtap/pico2/cdc_trx.hpp"
#include "lcdtap/pico2/json_intf.hpp"
#include "stats.hpp"

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
static JsonIntf gIf;

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
        "\"enableKeyId\":\"%s\",\"enableKeyValueMin\":0,"
        "\"enableKeyValueMax\":%d",
        static_cast<int>(*hc.currentOutIf),
        lcdtap::CONFIG_IDS[static_cast<int>(lcdtap::Configs::BUS_INTERFACE)],
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
                            lcdtap::BusType oldBus, bool save, void* ctx) {
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

  if (!needReboot) {
    // Re-init even when the bus type is unchanged: a controller-family or
    // I2C-address change must reach the slave peripheral.
    hc.switchIface(cfg.busInterface);
  }
  // Keep the live values in step when no reboot will do it for us, so a
  // getparams straight after Apply reports what was actually applied.
  if (!needReboot) {
    *hc.currentOutIf = newOutIf;
    *hc.currentDac = newDac;
  }

  // "save": false skips persistence, except when a reboot is needed: the
  // reboot re-reads flash, so an unsaved change would silently revert.
  if (save || needReboot) {
    ConfigFile toSave = {};
    toSave.libConfig = hc.inst->getConfig();
    toSave.outputInterface = static_cast<uint8_t>(newOutIf);
    toSave.compositeDac = static_cast<uint8_t>(newDac);
    hc.saveConfig(toSave);
  }

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

  JsonIntfCallbacks cb;
  cb.ctx = &gHostCtx;
  cb.numHostParams = NUM_HOST_PARAMS;
  cb.hostParamAnchorSlot = static_cast<int>(HOST_PARAM_ANCHOR);
  cb.buildHostParamChunk = uniBuildHostParamChunk;
  cb.beginSetParams = uniBeginSetParams;
  cb.stageHostParam = uniStageHostParam;
  cb.commitParams = uniCommitParams;
  cb.statsCollect = uniStatsCollect;
  cb.statsReset = uniStatsReset;

  cdcTrxInit();
  jsonIntfInit(&gIf, lcdtap, currentIface, cdcTrxTransport(), cb);
}

bool uartIfRebootPending() { return jsonIntfRebootPending(&gIf); }

bool uartIfRespIdle() { return jsonIntfRespIdle(&gIf); }

void uartIfProcess() { jsonIntfProcess(&gIf); }
