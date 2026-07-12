// LcdTap example for M5Stack Tab5 (ESP32-P4)
//
// Receives LCD controller commands as an SPI slave (PARLIO RX capture) or
// I2C slave (i2c_ll direct) on the back-side ports and displays the
// resulting framebuffer on the Tab5's 720x1280 MIPI-DSI panel via M5GFX,
// at the panel's native (portrait) orientation.
//
// Task layout:
//   core 0: PARLIO/I2C ISRs + inputTask (ring drain -> LcdTap)
//   core 1: displayTask (touch/IMU/OSD/keypad + strip rendering)
//
// The OSD is operated with a virtual touch keypad that follows the device
// orientation (BMI270). Configuration persists in NVS and is applied at
// boot; hold a touch on the panel during boot to start with defaults.

#include <M5Unified.h>

#include <cstdio>

#include <driver/gpio.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_cpu.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>

#include <lcdtap/lcdtap.hpp>
#include <lcdtap/osd.hpp>

#include "app_config.h"
#include "lcdtap/m5tab5/display_out.hpp"
#include "lcdtap/m5tab5/i2c_slave.hpp"
#include "lcdtap/m5tab5/imu_orient.hpp"
#include "lcdtap/m5tab5/keypad.hpp"
#include "lcdtap/m5tab5/nvs_config.hpp"
#include "lcdtap/m5tab5/parlio_spi_slave.hpp"
#include "lcdtap/m5tab5/ram_pool.hpp"

using namespace lcdtap::m5tab5;

namespace {
constexpr const char *TAG = "main";
}

//=============================================================================
// Buffers
//=============================================================================
static uint8_t spiStaging[SPI_STAGING_BYTES];
static uint32_t i2cRingBuf[I2C_RING_BUF_WORDS];

//=============================================================================
// Module state
//=============================================================================
static lcdtap::LcdTap *gInst = nullptr;
static lcdtap::Osd gOsd;
static ParlioSpiSlaveState gSpi;
static I2cSlaveState gI2c;
static DisplayOutState gDisp;
static KeypadState gKeypad;
static ImuOrientState gOrient;

// Always overwritten from cfg.busInterface early in app_main() before any
// task/ISR reads it; this initializer just documents the fallback value.
static lcdtap::BusType gCurrentIface = lcdtap::BusType::I2C;
static bool gIfaceActive = false;

static TaskHandle_t gInputTask = nullptr;

// RESX events recorded by the GPIO ISR, consumed by the input task.
static volatile bool gResxEvent = false;
static volatile bool gResxFell = false;

// Capture-recovery request from the display task's watchdog, consumed by
// the input task (realign must not race with ring/deserializer access).
static volatile bool gSpiRealignReq = false;

// I2C bus activity indicator: SDA/SCL levels are sampled in the input
// task loop (~1 kHz); level changes between samples increment this.
// Not an edge count — just proof that the master is toggling the bus.
static volatile uint32_t gI2cBusActivity = 0;

// Offscreen OSD raster (OSD_WIDTH x OSD_HEIGHT, RGB565). The OSD is
// rendered here once per frame and composited onto the panel rotated by
// the IMU orientation, so it always appears upright to the user.
// Written and read only by the display task.
static uint16_t *gOsdBuf = nullptr;
static bool gOsdVisible = false;
static uint8_t gOsdOrient = 0;

// Cumulative timing (microseconds, since boot) for the parts of the
// display task's per-frame loop outside of displayOutRenderFrame(), which
// has its own breakdown in DisplayOutState. Diffed against a periodic
// snapshot in displayTask() to report per-frame averages, same as fps.
static uint64_t gInputPollUs = 0;  // M5.update() + touch/IMU/keypad/OSD state
static uint64_t gOsdRasterUs = 0;  // Osd::fillScanline() loop into gOsdBuf

//=============================================================================
// Host interface (LcdTap controller framebuffer)
//=============================================================================
// Backed by a fixed-size PSRAM pool (ram_pool.hpp/.cpp), allocated once
// at boot by sramPoolInit() and sized for the largest buffer the OSD can
// configure. LcdTap calls alloc/free on every controller switch, but
// there is nothing to actually (re)allocate here -- the pool is handed
// out as-is and never freed, hence smartFree() being a no-op.
static void *smartAlloc(size_t size) { return frameBuffMemPool; }

static void smartFree(void *ptr) {}

static void hostLog(void *, const char *message) {
  ESP_LOGI("lcdtap", "%s", message);
}

static uint64_t millis64() { return (uint64_t)(esp_timer_get_time() / 1000); }

//=============================================================================
// RESX interrupt (deferred to the input task; LcdTap is not IRAM-safe)
//=============================================================================
static void IRAM_ATTR resxIsrHandler(void *) {
  // Resample the level instead of relying on edge flags so that a
  // simultaneous fall+rise pair cannot latch a stuck reset.
  if (!gpio_get_level((gpio_num_t)PIN_RESX)) gResxFell = true;
  gResxEvent = true;
  BaseType_t woken = pdFALSE;
  if (gInputTask) vTaskNotifyGiveFromISR(gInputTask, &woken);
  if (woken != pdFALSE) portYIELD_FROM_ISR();
}

//=============================================================================
// Interface switching
//=============================================================================
static void switchInterface(lcdtap::BusType newIface) {
  if (newIface != lcdtap::BusType::I2C &&
      newIface != lcdtap::BusType::SPI_4LINE) {
    ESP_LOGW(TAG, "unsupported bus interface %d; keeping current",
             (int)newIface);
    return;
  }

  if (gIfaceActive) {
    if (gCurrentIface == lcdtap::BusType::I2C) {
      i2cSlaveDeinit(&gI2c);
      // The PARLIO unit may be running as an I2C bus sniffer.
      if (gSpi.active) parlioSpiSlaveDeinit(&gSpi);
    } else {
      parlioSpiSlaveDeinit(&gSpi);
    }
    if (gInst) {
      gInst->inputReset(true);
      gInst->inputReset(false);
    }
  }
  gIfaceActive = true;

  if (newIface == lcdtap::BusType::I2C) {
    // Use the I2C port that M5Unified did not claim for the internal
    // touch/IMU bus.
    int internalPort = M5.In_I2C.getPort();
    int slavePort = internalPort == 0 ? 1 : 0;
    I2cSlaveConfig i2cCfg = {slavePort, PIN_I2C_SDA, PIN_I2C_SCL,
                             I2C_SLAVE_ADDR};
    gI2c.inst = gInst;
    gI2c.drainTask = gInputTask;
    esp_err_t err = i2cSlaveInit(&gI2c, i2cCfg, i2cRingBuf, I2C_RING_BUF_WORDS);
    ESP_LOGI(TAG,
             "i2c slave: port=%d (In_I2C port=%d) sda=%d scl=%d addr=0x%02X "
             "err=%d",
             slavePort, internalPort, PIN_I2C_SDA, PIN_I2C_SCL, I2C_SLAVE_ADDR,
             (int)err);

    if (I2C_SNIFF_ENABLE) {
      // Diagnostic bus sniffer: sample SDA on every SCL rising edge with
      // the PARLIO unit, in parallel with the I2C slave (input-only GPIO
      // matrix fan-out, does not disturb the bus). The raw dump then
      // shows the exact wire bit stream. CS lanes are tied to PIN_PRIME
      // (idles low = "in frame") so all bits are captured.
      ParlioSpiSlaveConfig sniffCfg = {PIN_I2C_SCL, PIN_I2C_SDA,
                                       PIN_I2C_SDA, PIN_PRIME,
                                       PIN_PRIME,   PIN_PRIME_DATA};
      gSpi.inst = nullptr;  // capture only; nothing is fed to LcdTap
      gSpi.drainTask = gInputTask;
      gSpi.rawDumpEnabled = true;
      err = parlioSpiSlaveInit(&gSpi, sniffCfg, SPI_DMA_BUF_BYTES, spiStaging,
                               SPI_STAGING_BYTES);
      ESP_LOGI(TAG, "i2c sniffer (parlio on SCL/SDA): err=%d", (int)err);
    }
  } else {
    ParlioSpiSlaveConfig spiCfg = {PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_DC,
                                   PIN_SPI_CS,  PIN_PRIME,    PIN_PRIME_DATA};
    gSpi.inst = gInst;
    gSpi.drainTask = gInputTask;
    gSpi.rawDumpEnabled = SPI_RAW_DUMP_ENABLE;
    esp_err_t err = parlioSpiSlaveInit(&gSpi, spiCfg, SPI_DMA_BUF_BYTES,
                                       spiStaging, SPI_STAGING_BYTES);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "parlioSpiSlaveInit failed: %d", (int)err);
    }
  }
  gCurrentIface = newIface;
}

//=============================================================================
// Input task (core 0)
//=============================================================================
static void inputTask(void *) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, 1);

    if (gResxEvent) {
      gResxEvent = false;
      // Apply the pending pre-reset stream first so it is interpreted in
      // the pre-reset controller state, then reset.
      if (gIfaceActive && gCurrentIface == lcdtap::BusType::SPI_4LINE) {
        parlioSpiSlaveProcess(&gSpi);
      }
      if (gResxFell) {
        gResxFell = false;
        gInst->inputReset(true);
        // Pre-reset frame tails stuck in the FIFO/DMA pipe would leak
        // out after the reset and permanently shift the address pointer
        // of masters that rely on exact auto-wrap (e.g. Arduboy never
        // re-sends SET_COL_ADDR). Inject a barrier to drop them — but
        // only while RESX is still low, which guarantees the bus is
        // quiet; late-processed bounce events must not cut real data.
        if (gCurrentIface == lcdtap::BusType::SPI_4LINE && gSpi.active &&
            !gpio_get_level((gpio_num_t)PIN_RESX)) {
          parlioSpiSlaveInjectBarrier(&gSpi);
        }
      }
      // Resample the level (instead of trusting edge flags) so that a
      // fall+rise pair processed late cannot latch a stuck reset.
      gInst->inputReset(!gpio_get_level((gpio_num_t)PIN_RESX));
    }

    if (gSpiRealignReq) {
      gSpiRealignReq = false;
      if (gCurrentIface == lcdtap::BusType::SPI_4LINE && gSpi.active) {
        parlioSpiSlaveRealign(&gSpi);
      }
    }

    if (gIfaceActive) {
      if (gCurrentIface == lcdtap::BusType::I2C) {
        i2cSlaveProcess(&gI2c);
        if (gSpi.active) parlioSpiSlaveProcess(&gSpi);  // bus sniffer drain
        // Bus activity sampling (diagnostics).
        static int prevSda = -1, prevScl = -1;
        int sda = gpio_get_level((gpio_num_t)PIN_I2C_SDA);
        int scl = gpio_get_level((gpio_num_t)PIN_I2C_SCL);
        if (sda != prevSda || scl != prevScl) {
          if (prevSda >= 0) gI2cBusActivity = gI2cBusActivity + 1;
          prevSda = sda;
          prevScl = scl;
        }
      } else {
        parlioSpiSlaveProcess(&gSpi);
      }
    }

    // Unconditional yield so IDLE0 (and its task-watchdog reset) and any
    // lower-priority core-0 task always get a scheduling window. This
    // task runs at INPUT_TASK_PRIO=20 and never yields voluntarily
    // otherwise: if PARLIO ISR notifications arrive faster than they can
    // be drained, ulTaskNotifyTake() above returns immediately every
    // time (the notification count never reaches zero), so without this
    // delay the task would never actually block, permanently starving
    // IDLE0 and tripping its watchdog (observed on hardware under a
    // sustained-overload capture scenario). CONFIG_FREERTOS_HZ=1000 on
    // this target, so 1 tick = 1ms; the chunk queue
    // (SPI_CHUNK_QUEUE_LEN=64, ~129us/chunk at the max 62.5MHz SCK rate)
    // absorbs several ms of backlog, and parlioSpiSlaveProcess() drains
    // the whole queue per call regardless of how much piled up, so this
    // costs at most ~1ms of added latency, not throughput.
    vTaskDelay(1);
  }
}

//=============================================================================
// Scanline / strip rendering callbacks (display task, core 1)
//=============================================================================
static void fillScanlineCb(uint16_t scanY, uint16_t *dst, void *) {
  gInst->fillScanline(scanY, dst);
  if (!gOsdVisible) return;

  // Composite the offscreen OSD raster, centered and rotated by the IMU
  // orientation so it reads upright from the user's point of view.
  // Coordinate conventions follow keypad.cpp's userToPanel: raster
  // u (user-right) / v (user-down) axes map onto the panel per orient.
  uint16_t screenW, screenH;
  gInst->getOutputScreenSize(&screenW, &screenH);
  const bool swapped = (gOsdOrient & 1) != 0;
  const uint16_t boxW = swapped ? lcdtap::OSD_HEIGHT : lcdtap::OSD_WIDTH;
  const uint16_t boxH = swapped ? lcdtap::OSD_WIDTH : lcdtap::OSD_HEIGHT;
  const uint16_t x0 = (screenW - boxW) / 2;
  const uint16_t y0 = (screenH - boxH) / 2;
  if (scanY < y0 || scanY >= (uint16_t)(y0 + boxH)) return;
  const uint16_t dy = scanY - y0;
  uint16_t *out = dst + x0;

  switch (gOsdOrient & 3u) {
    default:
    case 0:  // raster rows map to panel rows directly
      memcpy(out, gOsdBuf + (uint32_t)dy * lcdtap::OSD_WIDTH,
             (size_t)boxW * sizeof(uint16_t));
      break;
    case 2: {  // 180 degrees: reversed row order and direction
      const uint16_t *src =
          gOsdBuf +
          (uint32_t)(lcdtap::OSD_HEIGHT - 1 - dy) * lcdtap::OSD_WIDTH +
          (lcdtap::OSD_WIDTH - 1);
      for (uint16_t i = 0; i < boxW; ++i) *out++ = *src--;
      break;
    }
    case 1: {  // user's bottom = panel right: u -> -panelY, v -> +panelX
      const uint16_t *src = gOsdBuf + (lcdtap::OSD_WIDTH - 1 - dy);
      for (uint16_t i = 0; i < boxW; ++i) {
        *out++ = *src;
        src += lcdtap::OSD_WIDTH;
      }
      break;
    }
    case 3: {  // user's bottom = panel left: u -> +panelY, v -> -panelX
      const uint16_t *src =
          gOsdBuf + (uint32_t)(lcdtap::OSD_HEIGHT - 1) * lcdtap::OSD_WIDTH + dy;
      for (uint16_t i = 0; i < boxW; ++i) {
        *out++ = *src;
        src -= lcdtap::OSD_WIDTH;
      }
      break;
    }
  }
}

static void fillStripCb(uint16_t yTop, uint16_t numLines, uint16_t *strip,
                        void *) {
  keypadFillStrip(&gKeypad, yTop, numLines, strip, gDisp.width);
}

//=============================================================================
// Display / UI task (core 1)
//=============================================================================
static void displayTask(void *) {
  uint64_t lastStatsMs = millis64();
  uint32_t lastStatsFrames = 0;
  uint32_t lastIsrChunks = 0;
  uint64_t lastInputUs = 0, lastOsdRasterUs = 0;
  uint64_t lastWaitUs = 0, lastFillUs = 0, lastStripUs = 0, lastSubmitUs = 0,
           lastDrainUs = 0;
  uint64_t lastDmaBusyUs = 0;

  while (true) {
    uint64_t nowMs = millis64();
    int64_t tInput0 = esp_timer_get_time();

    M5.update();

    // Touch points in panel coordinates.
    KeypadTouch pts[4];
    int numPts = 0;
    int touchCount = M5.Touch.getCount();
    for (int i = 0; i < touchCount && numPts < 4; ++i) {
      auto det = M5.Touch.getDetail(i);
      if (det.isPressed()) {
        pts[numPts].x = det.x;
        pts[numPts].y = det.y;
        ++numPts;
      }
    }

    // IMU orientation.
    if (M5.Imu.update()) {
      float ax, ay, az;
      M5.Imu.getAccel(&ax, &ay, &az);
      imuOrientUpdate(&gOrient, nowMs, ax, ay, az);
    }

    uint8_t keys =
        keypadUpdate(&gKeypad, nowMs, pts, numPts, imuOrientGet(&gOrient));

    uint8_t action = gOsd.update(nowMs, *gInst, keys);
    if (action == lcdtap::OSD_ACTION_APPLY) {
      const lcdtap::OsdMenuItem *ifaceItem = nullptr;
      uint16_t id = lcdtap::OSD_ITEM_ID_SYS_BASE +
                    static_cast<uint16_t>(lcdtap::ConfigId::BUS_INTERFACE);
      gOsd.getItemById(id, &ifaceItem);
      lcdtap::BusType newIface =
          ifaceItem ? static_cast<lcdtap::BusType>(ifaceItem->config.value)
                    : gCurrentIface;

      ConfigFile toSave;
      toSave.version = CONFIG_FILE_VERSION;
      toSave.libConfig = gInst->getConfig();
      if (!saveConfig(toSave)) {
        ESP_LOGE(TAG, "saveConfig failed");
      }

      if (newIface != gCurrentIface) {
        switchInterface(newIface);
      }
    }

    int64_t tInput1 = esp_timer_get_time();
    gInputPollUs += tInput1 - tInput0;

    // Render the OSD into its offscreen raster and snapshot the values
    // the compositor (fillScanlineCb, same task) uses for this frame.
    gOsdOrient = imuOrientGet(&gOrient);
    gOsdVisible =
        gOsdBuf != nullptr && gOsd.getState() != lcdtap::OsdState::HIDDEN;
    if (gOsdVisible) {
      for (uint16_t row = 0; row < lcdtap::OSD_HEIGHT; ++row) {
        gOsd.fillScanline(row, gOsdBuf + (uint32_t)row * lcdtap::OSD_WIDTH);
      }
    }
    gOsdRasterUs += esp_timer_get_time() - tInput1;

    displayOutRenderFrame(&gDisp, fillScanlineCb, fillStripCb, nullptr);

    // Periodic diagnostics + capture watchdog.
    if (nowMs - lastStatsMs >= 5000) {
      uint32_t frames = gDisp.frameCount - lastStatsFrames;
      uint64_t elapsedMs = nowMs - lastStatsMs;
      float fps = frames * 1000.0f / (float)elapsedMs;
      // Moved up from the watchdog check below so both branches can use
      // the same snapshot.
      uint32_t isrChunks = gSpi.isrChunkCount;
      if (gCurrentIface == lcdtap::BusType::I2C) {
        char i2cStatus[160];
        i2cSlaveDebugStatus(&gI2c, i2cStatus, sizeof(i2cStatus));
        ESP_LOGI(TAG, "fps=%.1f iface=%d %s sda=%d scl=%d act=%lu", fps,
                 (int)gCurrentIface, i2cStatus,
                 gpio_get_level((gpio_num_t)PIN_I2C_SDA),
                 gpio_get_level((gpio_num_t)PIN_I2C_SCL),
                 (unsigned long)gI2cBusActivity);
      } else {
        ESP_LOGI(TAG,
                 "fps=%.1f iface=%d spiOvf=%lu desync=%lu csFrames=%lu "
                 "bitDrop=%lu isrChunks=%lu rx=%lu forcedRecovery=%lu",
                 fps, (int)gCurrentIface, (unsigned long)gSpi.ringOverflowCount,
                 (unsigned long)gSpi.isrDesyncCount,
                 (unsigned long)gSpi.deser.frameStartCount,
                 (unsigned long)gSpi.deser.partialBitDropCount,
                 (unsigned long)gSpi.isrChunkCount,
                 (unsigned long)gSpi.deser.emitCount,
                 (unsigned long)gSpi.deser.forcedRecoveryCount);
      }

      // Per-frame timing breakdown, to see which stage of the display
      // pipeline is the bottleneck. "wait" is time blocked waiting for a
      // strip buffer's previous async (or pushImage) transfer to finish --
      // large wait means the DMA/panel transfer is the bottleneck, not the
      // CPU; large fill/osdRaster/input means the CPU compute is.
      if (frames > 0) {
        float frameUs = fps > 0.0f ? 1000000.0f / fps : 0.0f;
        float avgInput = (float)(gInputPollUs - lastInputUs) / frames;
        float avgOsdRaster = (float)(gOsdRasterUs - lastOsdRasterUs) / frames;
        float avgWait = (float)(gDisp.waitUs - lastWaitUs) / frames;
        float avgFill = (float)(gDisp.fillUs - lastFillUs) / frames;
        float avgStrip = (float)(gDisp.stripUs - lastStripUs) / frames;
        float avgSubmit = (float)(gDisp.submitUs - lastSubmitUs) / frames;
        float avgDrain = (float)(gDisp.drainUs - lastDrainUs) / frames;
        // Ground-truth DMA execution time (submit to completion callback)
        // -- unlike wait/drain, this isn't inflated or hidden by however
        // much CPU work happened to overlap it.
        float avgDmaBusy = (float)(gDisp.blit.busyUs - lastDmaBusyUs) / frames;
        auto pct = [&](float us) {
          return frameUs > 0.0f ? 100.0f * us / frameUs : 0.0f;
        };
        ESP_LOGI(TAG,
                 "timing us/frame: input=%.0f(%.0f%%) osdRaster=%.0f(%.0f%%) "
                 "wait=%.0f(%.0f%%) fill=%.0f(%.0f%%) strip=%.0f(%.0f%%) "
                 "submit=%.0f(%.0f%%) drain=%.0f(%.0f%%) dmaBusy=%.0f(%.0f%%)",
                 avgInput, pct(avgInput), avgOsdRaster, pct(avgOsdRaster),
                 avgWait, pct(avgWait), avgFill, pct(avgFill), avgStrip,
                 pct(avgStrip), avgSubmit, pct(avgSubmit), avgDrain,
                 pct(avgDrain), avgDmaBusy, pct(avgDmaBusy));
        lastInputUs = gInputPollUs;
        lastOsdRasterUs = gOsdRasterUs;
        lastWaitUs = gDisp.waitUs;
        lastFillUs = gDisp.fillUs;
        lastStripUs = gDisp.stripUs;
        lastSubmitUs = gDisp.submitUs;
        lastDmaBusyUs = gDisp.blit.busyUs;
        lastDrainUs = gDisp.drainUs;
      }

      // If no DMA chunk arrived for a whole stats period, the capture is
      // either idle (master quiet — realign is harmless then) or dead
      // (recovered by the realign). Handled by the input task. isrChunks
      // was snapshotted above (shared with the Phase 0 SCK estimate).
      if (gCurrentIface == lcdtap::BusType::SPI_4LINE && gSpi.active &&
          isrChunks == lastIsrChunks) {
        gSpiRealignReq = true;
      }
      lastIsrChunks = isrChunks;
      // Raw-sample dump for bring-up: print once when new bytes arrived.
      static uint16_t prevDumpLen = 0;
      if (gSpi.rawDumpLen < prevDumpLen) prevDumpLen = 0;  // re-armed
      if (gSpi.rawDumpEnabled && gSpi.rawDumpLen != prevDumpLen) {
        prevDumpLen = gSpi.rawDumpLen;
        // Built into one buffer (rather than one ESP_LOGx call per byte)
        // so the hex dump stays on a single log line.
        char dumpLine[24 + sizeof(gSpi.rawDumpBuf) * 3];
        int pos = snprintf(dumpLine, sizeof(dumpLine),
                           "raw dump (%u bytes):", (unsigned)prevDumpLen);
        for (uint16_t i = 0; i < prevDumpLen && pos < (int)sizeof(dumpLine);
             ++i) {
          pos += snprintf(dumpLine + pos, sizeof(dumpLine) - pos, " %02X",
                          gSpi.rawDumpBuf[i]);
        }
        ESP_LOGI(TAG, "%s", dumpLine);
      }
      lastStatsMs = nowMs;
      lastStatsFrames = gDisp.frameCount;
    }

    // Yield so the core-1 idle task (watchdog) can run.
    vTaskDelay(1);
  }
}

//=============================================================================
// Phase 1-1 diagnostic: unaligned-load micro-benchmark
// (example/m5tab5/tmp.improve-performance.md; only runs when
// BENCH_UNALIGNED_LOAD is true). Determines whether the ESP32-P4 HP core
// executes a plain, potentially-misaligned 32-bit pointer-cast load
// (*reinterpret_cast<const uint32_t*>(p) for arbitrary p -- UB in the
// standard) at native speed, at slow trap-emulated speed, or faults
// outright. Deliberately called before M5.begin() so no ISR/task exists
// yet to add jitter to the cycle counts, and deliberately logs + flushes
// before each offset's trial so a crash mid-benchmark still leaves a
// clear trace of which offset caused it.
//=============================================================================
static void benchUnalignedLoad() {
  constexpr uint32_t BUF_BYTES = 16u * 1024u;
  constexpr int TRIALS = 8;
  uint8_t *buf = (uint8_t *)heap_caps_malloc(
      BUF_BYTES + 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf) {
    ESP_LOGE(TAG, "[bench] alloc failed");
    return;
  }
  for (uint32_t i = 0; i < BUF_BYTES + 4; ++i) {
    buf[i] = (uint8_t)(i * 2654435761u);
  }

  uint32_t alignedCy = 0;
  for (int off = 0; off < 4; ++off) {
    ESP_LOGI(TAG, "[bench] off=%d starting", off);
    fflush(stdout);  // last line before a potential fault at this offset
    uint32_t best = 0xFFFFFFFFu;
    for (int t = 0; t < TRIALS; ++t) {
      volatile uint32_t acc = 0;
      uint32_t t0 = esp_cpu_get_cycle_count();
      for (const uint8_t *p = buf + off; p + 4 <= buf + off + BUF_BYTES;
           p += 4) {
        acc += *reinterpret_cast<const uint32_t *>(p);
      }
      uint32_t dt = esp_cpu_get_cycle_count() - t0;
      if (dt < best) best = dt;
      (void)acc;
    }
    uint32_t words = BUF_BYTES / 4;
    float cyPerWord = (float)best / words;
    if (off == 0) alignedCy = best;
    float ratio = alignedCy > 0 ? (float)best / alignedCy : 0.0f;
    ESP_LOGI(TAG, "[bench] off=%d best=%lu cy, %.2f cy/word, ratio=%.2fx", off,
             (unsigned long)best, cyPerWord, ratio);
    fflush(stdout);
  }
  heap_caps_free(buf);
  ESP_LOGI(TAG, "[bench] done");
  fflush(stdout);
}

//=============================================================================
// app_main
//=============================================================================
extern "C" void app_main(void) {
  // Never block on USB CDC writes: when the host stops draining the CDC
  // buffer, blocking log/printf calls would periodically freeze the
  // calling task (log messages are best-effort diagnostics anyway).
  usb_serial_jtag_vfs_use_nonblocking();

  esp_err_t nvsErr = nvs_flash_init();
  if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvsErr = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvsErr);

  if (sramPoolInit() != ESP_OK) {
    ESP_LOGE(TAG, "sramPoolInit failed");
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
  }

  if (BENCH_UNALIGNED_LOAD) {
    // Give a terminal a moment to attach before the benchmark's log lines
    // -- run before M5.begin() so no ISR/task exists yet to add jitter --
    // are still diagnosable via the last-printed offset on a crash. Only
    // reorders boot for this dedicated diagnostic build; normal builds
    // are unaffected.
    vTaskDelay(pdMS_TO_TICKS(1000));
    benchUnalignedLoad();
  }

  auto m5cfg = M5.config();
  M5.begin(m5cfg);
  vTaskDelay(pdMS_TO_TICKS(1000));  // give a terminal a moment to attach

  // Native physical orientation (portrait, 720x1280) -- matching logical
  // and physical orientation lets M5GFX's fast contiguous-memcpy blit
  // path apply directly, avoiding the slow generic per-pixel rotate path
  // that a landscape setRotation(1/3) would require. OSD/keypad rotation
  // (imu_orient.cpp's ORIENT_CORRECTION) was re-tuned on hardware to match.
  M5.Display.setRotation(0);
  M5.Display.setColorDepth(16);

  ESP_LOGI(TAG,
           "LcdTap M5Tab5  PSRAM=%u internalFree=%u board=%d display=%dx%d",
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (int)M5.getBoard(), M5.Display.width(), M5.Display.height());

  // M5.begin() runs M5.Imu.begin() very early (before Display.init() and
  // other setup that takes noticeable time); the BMI270 has occasionally
  // been observed not yet ready at that point, leaving M5.Imu disabled
  // for the rest of the session. Retry the (idempotent, side-effect-free
  // on failure) begin() call now that more time has passed.
  for (int attempt = 0; !M5.Imu.isEnabled() && attempt < 3; ++attempt) {
    if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(50));
    M5.Imu.begin(&M5.In_I2C, M5.getBoard());
  }

  // Hold a touch on the panel during boot to start with default settings
  // (the Tab5 has no physical buttons).
  M5.update();
  bool forceDefaults = M5.Touch.getCount() > 0;

  ConfigFile savedCfg = {};
  bool hasSavedCfg = !forceDefaults && loadConfig(&savedCfg);

  // ---------------------------------------------------------------------
  // LcdTap init
  // ---------------------------------------------------------------------
  // The display is kept in portrait orientation (setRotation(0)) for DMA
  // efficiency, but we want to present landscape as the default orientation
  // to the user. Apply a rotation offset to the preset configuration to
  // achieve this user-facing orientation while maintaining the physical
  // portrait layout for optimal DMA performance.
  lcdtap::setPresetRotationOffset(3);

  // Default target: SSD1306 over I2C, a low-data-rate display well within
  // this capture pipeline's throughput headroom (see
  // tmp.improve-performance.md for why higher-rate SPI masters like
  // PicoSystem are not reliably supported on this hardware).
  lcdtap::LcdTapConfig cfg;
  lcdtap::getPresetConfig(lcdtap::ConfigPreset::SSD1306, &cfg);
  cfg.dviWidth = M5.Display.width();
  cfg.dviHeight = M5.Display.height();

  if (hasSavedCfg) {
    savedCfg.libConfig.dviWidth = cfg.dviWidth;
    savedCfg.libConfig.dviHeight = cfg.dviHeight;
    cfg = savedCfg.libConfig;
  }
  // Always derive gCurrentIface from cfg.busInterface (default or saved),
  // not just when a saved config was loaded -- otherwise a first boot
  // would keep gCurrentIface at its static-initialized value regardless
  // of what the default config's busInterface actually is.
  gCurrentIface = cfg.busInterface;
  if (gCurrentIface != lcdtap::BusType::I2C &&
      gCurrentIface != lcdtap::BusType::SPI_4LINE) {
    gCurrentIface = lcdtap::BusType::SPI_4LINE;
  }

  lcdtap::HostInterface host;
  host.alloc = smartAlloc;
  host.free = smartFree;
  host.log = hostLog;
  host.userData = nullptr;

  static lcdtap::LcdTap inst(cfg, host);
  if (inst.getStatus() != lcdtap::Status::OK) {
    ESP_LOGE(TAG, "LcdTap init failed: %d", (int)inst.getStatus());
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  gInst = &inst;

  // ---------------------------------------------------------------------
  // OSD / keypad / IMU / display output
  // ---------------------------------------------------------------------
  lcdtap::OsdConfig osdCfg;
  lcdtap::getDefaultOsdConfig(&osdCfg);
  gOsd.init(osdCfg);

  // Offscreen OSD raster for orientation-following compositing. PSRAM,
  // not internal SRAM: competing with the DMA engine's own working
  // memory (async_blit.cpp) for internal SRAM was measured to starve
  // GDMA's link-list allocation ("no mem for link list items") and
  // corrupt the display. PSRAM is an acceptable place for this buffer
  // since it's touched only while the OSD is open, not every frame.
  {
    size_t osdBytes =
        (size_t)lcdtap::OSD_WIDTH * lcdtap::OSD_HEIGHT * sizeof(uint16_t);
    gOsdBuf = (uint16_t *)heap_caps_malloc(osdBytes,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gOsdBuf) {
      ESP_LOGE(TAG, "OSD raster allocation failed");
    }
  }

  keypadInit(&gKeypad, M5.Display.width(), M5.Display.height());
  imuOrientInit(&gOrient);

  DisplayOutConfig dispCfg = {STRIP_LINES};
  if (!displayOutInit(&gDisp, &M5.Display, dispCfg)) {
    ESP_LOGE(TAG, "displayOutInit failed (strip buffer)");
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
  }

  // ---------------------------------------------------------------------
  // RESX input (pull-up, any-edge interrupt)
  // ---------------------------------------------------------------------
  gpio_config_t resxCfg = {};
  resxCfg.pin_bit_mask = 1ull << PIN_RESX;
  resxCfg.mode = GPIO_MODE_INPUT;
  resxCfg.pull_up_en = GPIO_PULLUP_ENABLE;
  resxCfg.intr_type = GPIO_INTR_ANYEDGE;
  gpio_config(&resxCfg);
  gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  gpio_isr_handler_add((gpio_num_t)PIN_RESX, resxIsrHandler, nullptr);

  // ---------------------------------------------------------------------
  // Tasks (input on core 0, display/UI on core 1)
  // ---------------------------------------------------------------------
  xTaskCreatePinnedToCore(inputTask, "lcdtap_input", INPUT_TASK_STACK, nullptr,
                          INPUT_TASK_PRIO, &gInputTask, INPUT_TASK_CORE);

  switchInterface(gCurrentIface);

  xTaskCreatePinnedToCore(displayTask, "lcdtap_disp", DISPLAY_TASK_STACK,
                          nullptr, DISPLAY_TASK_PRIO, nullptr,
                          DISPLAY_TASK_CORE);

  ESP_LOGI(TAG, "LcdTap ready. iface=%d", (int)gCurrentIface);
}
