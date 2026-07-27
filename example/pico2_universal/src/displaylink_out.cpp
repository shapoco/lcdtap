#include "displaylink_out.hpp"

#include <cstring>

#include "hardware/gpio.h"
#include "pico/multicore.h"

#include "usb_disp.h"
#include "usb_disp_udh_host.h"

#include "displaylink_map.hpp"

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

// Hotplug/enumeration poll cadence. Enumeration itself blocks Core 0 for up
// to a few hundred ms; that input glitch on adapter hotplug is accepted.
static constexpr uint32_t POLL_INTERVAL_MS = 10;

// Frame tick for OSD key handling / LED blink.
static constexpr uint32_t FRAME_TICK_MS = 33;

// Minimum interval between full OSD-rectangle refreshes while the menu is
// open, so the menu stays responsive without monopolizing the bulk link.
static constexpr uint32_t OSD_REFRESH_MS = 100;

// Output lines generated (fillScanline + RLE encode) per process() call.
// Bounds the time Core 0 spends away from the SPI/I2C ring buffers.
static constexpr uint32_t MAX_FILLS_PER_CALL = 8;

// Must match USB_DISP_UDH_RING_SIZE in usb_disp_udh_host.cpp (not exported).
static constexpr uint32_t UDH_RING_CAPACITY = 16384;

// Duplicate output lines could be produced with the COPY16 blit instead of
// re-sending pixels, but on real DL hardware the blit engine runs
// asynchronously from the command parser: with small RLE updates and many
// back-to-back copies (a 1bpp source like the SSD1306 upscaled ~10x is the
// worst case) the blit races the surrounding updates and leaves stale
// fragments — seen as thin lines at update-span edges. Default to plain
// re-sends; the copy path is kept for experiments only.
#ifndef DISPLAYLINK_USE_COPY16
#define DISPLAYLINK_USE_COPY16 1
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline usb_disp_t* D(DisplaylinkOutState* s) {
  return static_cast<usb_disp_t*>(s->disp);
}

static inline uint32_t nowMs() { return to_ms_since_boot(get_absolute_time()); }

// Worst-case DL RLE bytes for a w-pixel span (see USB_DISP_DL_LINE_MAX).
static inline uint32_t worstLineBytes(uint32_t w) {
  return 7u * ((w + 255u) / 256u) + 3u * w + 16u;
}

// True when the bulk ring can absorb one worst-case line plus some copy
// commands without usb_disp_update_565() spinning on Core 0.
static bool ringHasRoom(DisplaylinkOutState* s, uint32_t spanW) {
  uint32_t pending = usb_disp_udh_host_bulk_pending(usb_disp_get_host(D(s)));
  return pending + worstLineBytes(spanW) + 512u <= UDH_RING_CAPACITY;
}

static void fillLine(DisplaylinkOutState* s, uint32_t y) {
  s->inst->fillScanline(static_cast<uint16_t>(y), s->lineBuf);
  if (s->fillCb) s->fillCb(static_cast<uint16_t>(y), s->lineBuf, s->fillUser);
}

// A failed send means the device did not (fully) receive pixels whose dirty
// bits were already claimed; the failure is latched and process() schedules
// a full repaint so nothing is lost once the link works again.
static void sendSpan(DisplaylinkOutState* s, uint32_t y, uint32_t x0,
                     uint32_t x1) {
  uint32_t w = x1 - x0 + 1u;
  if (!usb_disp_update_565(D(s), static_cast<uint16_t>(x0),
                           static_cast<uint16_t>(y), static_cast<uint16_t>(w),
                           1u, s->lineBuf + x0, w)) {
    s->sendFailed = true;
  }
  s->needFlush = true;
}

#if DISPLAYLINK_USE_COPY16
static void copySpan(DisplaylinkOutState* s, uint32_t x0, uint32_t x1,
                     uint32_t ySrc, uint32_t yDst) {
  if (!usb_disp_copy(D(s), static_cast<uint16_t>(x0),
                     static_cast<uint16_t>(ySrc), static_cast<uint16_t>(x0),
                     static_cast<uint16_t>(yDst),
                     static_cast<uint16_t>(x1 - x0 + 1u), 1u)) {
    s->sendFailed = true;
  }
}
#endif

// Output-mapping helpers (groupFirstLine / srcRangeToSpan) live in
// displaylink_map.hpp so the host-side span test exercises the same code.

// Continue an in-flight duplicate-line group: send remaining rows while the
// ring and the fill budget allow. The group's dirty bits are already
// claimed, so the group persists across process() calls until every row went
// out. Returns the number of fills consumed.
static uint32_t pumpPendingGroup(DisplaylinkOutState* s, uint32_t maxFills) {
  if (!s->groupActive) return 0;
  uint32_t fills = 0;
  const uint32_t x0 = s->groupX0;
  const uint32_t x1 = s->groupX1;
  while (s->groupNextY <= s->groupEndY) {
    if (fills >= maxFills) return fills;
    if (!ringHasRoom(s, x1 - x0 + 1u)) return fills;
    if (s->groupPerLine) {
      fillLine(s, s->groupNextY);
      ++fills;
    } else if (s->groupNeedFill) {
      fillLine(s, s->groupFillLine);
      s->groupNeedFill = false;
      ++fills;
    }
    sendSpan(s, s->groupNextY, x0, x1);
    ++s->groupNextY;
    if (!s->groupPerLine) ++fills;  // per-row RLE encode is fill-like work
  }
  s->groupActive = false;
  return fills;
}

// Send one group of duplicate output lines. Lines under the OSD overlay are
// not vertical duplicates of each other (the OSD renders per-line content on
// top), so overlapping groups are generated line by line. Returns fills
// consumed; when the budget or the ring runs out mid-group, the remainder
// stays pending (s->groupActive) and is finished by pumpPendingGroup().
static uint32_t sendGroup(DisplaylinkOutState* s, uint32_t y0, uint32_t y1,
                          uint32_t x0, uint32_t x1, uint32_t maxFills) {
  const bool overlapsOsd =
      s->osdActive && y1 > y0 && s->osdW != 0 && y1 >= s->osdY &&
      y0 <= static_cast<uint32_t>(s->osdY + s->osdH) - 1u && x1 >= s->osdX &&
      x0 <= static_cast<uint32_t>(s->osdX + s->osdW) - 1u;
#if DISPLAYLINK_USE_COPY16
  if (overlapsOsd) {
    for (uint32_t y = y0; y <= y1; ++y) {
      fillLine(s, y);
      sendSpan(s, y, x0, x1);
    }
    return y1 - y0 + 1u;
  }
  fillLine(s, y0);
  sendSpan(s, y0, x0, x1);
  for (uint32_t y = y0 + 1u; y <= y1; ++y) copySpan(s, x0, x1, y0, y);
  (void)maxFills;
  return 1;
#else
  s->groupActive = true;
  s->groupPerLine = overlapsOsd;
  s->groupNeedFill = !overlapsOsd;
  s->groupFillLine = y0;
  s->groupNextY = y0;
  s->groupEndY = y1;
  s->groupX0 = x0;
  s->groupX1 = x1;
  return pumpPendingGroup(s, maxFills);
#endif
}

// Start (or restart) a full repaint. The pending dirt is claimed here, at
// the start: the sweep resends everything with current framebuffer content
// anyway, while dirt that arrives during the multi-call sweep may cover
// regions the sweep has already passed and therefore must survive until the
// incremental pump picks it up. Clearing the map at sweep *completion* would
// silently drop such updates — and because hosts rewrite identical pixel
// values every frame, the value compare would never mark them again,
// leaving permanently stale lines on screen.
static void startFullRepaint(DisplaylinkOutState* s) {
  memset(s->inst->dirtyMap(), 0, s->inst->getConfig().buffHeight);
  s->fullRepaint = true;
  s->sweepY = 0;
  s->dirtyRow = 0;
  s->aggValid = false;
  s->aggPending = 0;
  // A pending duplicate-line group is superseded: the sweep covers its rows.
  s->groupActive = false;
}

// ---------------------------------------------------------------------------
// Full repaint sweep (connect / presentation-epoch change): every output
// line including the black borders, grouped by duplicate content.
// ---------------------------------------------------------------------------

// Returns the number of fills consumed (0 = out of ring space, try later).
static uint32_t sweepStep(DisplaylinkOutState* s,
                          const lcdtap::OutputMapInfo& mi, uint32_t maxFills) {
  const uint32_t outH = s->outH;
  const uint32_t outW = s->outW;
  if (s->sweepY >= outH) return 0;
  if (!ringHasRoom(s, outW)) return 0;

  const uint32_t y0 = s->sweepY;
  auto lineKey = [&](uint32_t y) -> int32_t {
    // Border and blanked lines are identical black -> shared key -1.
    if (mi.blanked || mi.destW == 0 || mi.destH == 0 || y < mi.destY ||
        y >= static_cast<uint32_t>(mi.destY + mi.destH)) {
      return -1;
    }
    return static_cast<int32_t>(
        (static_cast<uint64_t>(y - mi.destY) * mi.stepV) >> 16);
  };

  const int32_t key = lineKey(y0);
  uint32_t y1 = y0;
  // Cap the group length per step so one call never queues hundreds of
  // commands (borders can span most of the screen).
  while (y1 + 1u < outH && (y1 + 1u - y0) < 64u && lineKey(y1 + 1u) == key) {
    ++y1;
  }
  uint32_t fills = sendGroup(s, y0, y1, 0u, outW - 1u, maxFills);
  // The remainder of a pending group is finished by pumpPendingGroup();
  // the sweep cursor can advance regardless.
  s->sweepY = y1 + 1u;

  if (s->sweepY >= outH) {
    // The dirty map was claimed when the repaint started (startFullRepaint);
    // dirt that accumulated during the sweep stays for the incremental pump.
    // Without dirty tracking (oversized framebuffer) fall back to a
    // continuous rolling refresh.
    s->fullRepaint = !s->inst->isDirtyTrackingActive();
    s->sweepY = 0;
  }
  return fills > 0 ? fills : 1u;
}

// ---------------------------------------------------------------------------
// Incremental update, rot=0/2: one dirty fb row -> one group of output
// lines; segment bits -> x spans.
// ---------------------------------------------------------------------------

static uint32_t dirtyStepRot02(DisplaylinkOutState* s,
                               const lcdtap::OutputMapInfo& mi,
                               uint32_t maxFills) {
  const uint16_t fbH = mi.fbHeight;
  const uint16_t fbW = mi.fbWidth;
  uint8_t* map = s->inst->dirtyMap();
  uint32_t fills = 0;

  // Find the next dirty row from the cursor (bounded scan; the map is at
  // most 512 bytes so a full revolution per call is fine).
  for (uint32_t scanned = 0; scanned < fbH; ++scanned) {
    uint32_t r = s->dirtyRow;
    s->dirtyRow = static_cast<uint16_t>((r + 1u) % fbH);
    uint8_t bits = map[r];
    if (bits == 0) continue;

    if (fills >= maxFills || !ringHasRoom(s, mi.destW)) {
      s->dirtyRow = static_cast<uint16_t>(r);  // retry this row next call
      return fills;
    }
    map[r] = 0;  // claim before reading pixels: later writes re-mark

    // Rows outside the crop never reach the output.
    if (r < mi.srcY || r >= static_cast<uint32_t>(mi.srcY + mi.srcH)) {
      continue;
    }
    const uint32_t t =
        (mi.rotation == 0)
            ? (r - mi.srcY)
            : (static_cast<uint32_t>(mi.srcH) - 1u - (r - mi.srcY));
    uint32_t y0 = groupFirstLine(mi, t);
    uint32_t y1 = groupFirstLine(mi, t + 1u);
    y1 = (y1 > y0) ? y1 - 1u : y0;
    const uint32_t yMax = static_cast<uint32_t>(mi.destY + mi.destH) - 1u;
    if (y0 > yMax) continue;
    if (y1 > yMax) y1 = yMax;

    for (uint32_t b = 0; b < 8u; ++b) {
      if (!(bits & (1u << b))) continue;
      // Merge the run of consecutive set bits into one span.
      uint32_t bEnd = b;
      while (bEnd + 1u < 8u && (bits & (1u << (bEnd + 1u)))) ++bEnd;
      uint32_t c0 = b << 6;
      uint32_t c1 = (bEnd << 6) + 63u;
      if (c1 > static_cast<uint32_t>(fbW) - 1u) c1 = fbW - 1u;
      b = bEnd;
      // Clip to the crop and make crop-relative.
      const uint32_t cropEnd = static_cast<uint32_t>(mi.srcX + mi.srcW) - 1u;
      if (c0 < mi.srcX) c0 = mi.srcX;
      if (c1 > cropEnd) c1 = cropEnd;
      if (c0 > c1) continue;
      uint32_t x0, x1;
      if (!srcRangeToSpan(mi, c0 - mi.srcX, c1 - mi.srcX, mi.rotation == 2, &x0,
                          &x1)) {
        continue;
      }
      fills += sendGroup(s, y0, y1, x0, x1, maxFills - fills);
      if (s->groupActive || fills >= maxFills) {
        // Out of ring space or budget mid-row. The current group finishes
        // via pumpPendingGroup(); un-claim the segment runs not yet sent so
        // they are re-processed next call.
        map[r] |= static_cast<uint8_t>(bits & ~((2u << bEnd) - 1u));
        return fills;
      }
    }
  }
  return fills;
}

// ---------------------------------------------------------------------------
// Incremental update, rot=1/3: fb columns map to output lines and fb rows
// map to output x. The dirty map is claimed into per-segment row ranges
// (aggRowMin/Max); each 64-column segment then drives a range of output
// lines whose x span is the hull of its dirty rows.
// ---------------------------------------------------------------------------

static void claimAggregates(DisplaylinkOutState* s,
                            const lcdtap::OutputMapInfo& mi) {
  uint8_t* map = s->inst->dirtyMap();
  uint8_t all = 0;
  for (uint32_t k = 0; k < 8u; ++k) {
    s->aggRowMin[k] = 0xFFFFu;
    s->aggRowMax[k] = 0;
  }
  for (uint32_t r = 0; r < mi.fbHeight; ++r) {
    uint8_t bits = map[r];
    if (bits == 0) continue;
    map[r] = 0;
    all |= bits;
    for (uint32_t k = 0; k < 8u; ++k) {
      if (!(bits & (1u << k))) continue;
      if (r < s->aggRowMin[k]) s->aggRowMin[k] = static_cast<uint16_t>(r);
      if (r > s->aggRowMax[k]) s->aggRowMax[k] = static_cast<uint16_t>(r);
    }
  }
  s->aggPending = all;
  s->aggSeg = 0;
  s->aggCol = 0xFFFFFFFFu;
  s->aggValid = (all != 0);
}

static uint32_t dirtyStepRot13(DisplaylinkOutState* s,
                               const lcdtap::OutputMapInfo& mi,
                               uint32_t maxFills) {
  if (!s->aggValid) {
    claimAggregates(s, mi);
    if (!s->aggValid) return 0;
  }
  uint32_t fills = 0;
  const uint32_t cropColEnd = static_cast<uint32_t>(mi.srcX + mi.srcW) - 1u;
  const uint32_t cropRowEnd = static_cast<uint32_t>(mi.srcY + mi.srcH) - 1u;
  const uint32_t yMax = static_cast<uint32_t>(mi.destY + mi.destH) - 1u;

  while (s->aggPending != 0 && fills < maxFills) {
    const uint32_t k = s->aggSeg;
    if (k >= 8u) break;
    if (!(s->aggPending & (1u << k))) {
      ++s->aggSeg;
      continue;
    }
    // fb columns of this segment, clipped to the crop.
    uint32_t c0 = k << 6;
    uint32_t c1 = c0 + 63u;
    if (c0 < mi.srcX) c0 = mi.srcX;
    if (c1 > cropColEnd) c1 = cropColEnd;
    // Dirty row hull of this segment, clipped to the crop.
    uint32_t r0 = s->aggRowMin[k];
    uint32_t r1 = s->aggRowMax[k];
    if (r0 < mi.srcY) r0 = mi.srcY;
    if (r1 > cropRowEnd) r1 = cropRowEnd;
    if (c0 > c1 || r0 > r1) {
      s->aggPending &= static_cast<uint8_t>(~(1u << k));
      ++s->aggSeg;
      s->aggCol = 0xFFFFFFFFu;
      continue;
    }
    uint32_t x0, x1;
    if (!srcRangeToSpan(mi, r0 - mi.srcY, r1 - mi.srcY, mi.rotation == 1, &x0,
                        &x1)) {
      s->aggPending &= static_cast<uint8_t>(~(1u << k));
      ++s->aggSeg;
      s->aggCol = 0xFFFFFFFFu;
      continue;
    }

    if (s->aggCol == 0xFFFFFFFFu) s->aggCol = c0;
    while (s->aggCol <= c1 && fills < maxFills) {
      if (!ringHasRoom(s, x1 - x0 + 1u)) return fills;
      const uint32_t c = s->aggCol;
      const uint32_t t =
          (mi.rotation == 1)
              ? (c - mi.srcX)
              : (static_cast<uint32_t>(mi.srcW) - 1u - (c - mi.srcX));
      uint32_t y0 = groupFirstLine(mi, t);
      uint32_t y1 = groupFirstLine(mi, t + 1u);
      y1 = (y1 > y0) ? y1 - 1u : y0;
      ++s->aggCol;
      if (y0 <= yMax) {
        if (y1 > yMax) y1 = yMax;
        fills += sendGroup(s, y0, y1, x0, x1, maxFills - fills);
        if (s->groupActive) {
          // The rest of this column's group finishes via pumpPendingGroup();
          // the column cursor has already advanced.
          return fills;
        }
      }
    }
    if (s->aggCol > c1) {
      s->aggPending &= static_cast<uint8_t>(~(1u << k));
      ++s->aggSeg;
      s->aggCol = 0xFFFFFFFFu;
    }
  }
  if (s->aggPending == 0) s->aggValid = false;
  return fills;
}

// ---------------------------------------------------------------------------
// OSD overlay refresh: the OSD is composited onto the generated lines in
// output space, so its rectangle is resent independently of fb dirt.
// ---------------------------------------------------------------------------

static uint32_t osdStep(DisplaylinkOutState* s, uint32_t maxFills) {
  if (!s->osdSweepActive) return 0;
  uint32_t x0 = s->osdX;
  uint32_t x1 = static_cast<uint32_t>(s->osdX + s->osdW) - 1u;
  if (x1 > static_cast<uint32_t>(s->outW) - 1u) x1 = s->outW - 1u;
  const uint32_t yEnd = static_cast<uint32_t>(s->osdY + s->osdH) - 1u;

  uint32_t fills = 0;
  while (fills < maxFills && s->osdSweepY <= yEnd && s->osdSweepY < s->outH) {
    if (!ringHasRoom(s, x1 - x0 + 1u)) return fills;
    fillLine(s, s->osdSweepY);
    sendSpan(s, s->osdSweepY, x0, x1);
    ++s->osdSweepY;
    ++fills;
  }
  if (s->osdSweepY > yEnd || s->osdSweepY >= s->outH) {
    s->osdSweepActive = false;
    s->nextOsdMs = nowMs() + OSD_REFRESH_MS;
  }
  return fills;
}

// ---------------------------------------------------------------------------
// Connection / mode management
// ---------------------------------------------------------------------------

static void setupMode(DisplaylinkOutState* s) {
  usb_disp_t* d = D(s);
  bool ok = usb_disp_set_mode(d, s->cfg.width, s->cfg.height);
  if (!ok && !(s->cfg.width == 1280 && s->cfg.height == 720)) {
    // 1080p needs DL-165/195; smaller chips fall back to 720p.
    ok = usb_disp_set_mode(d, 1280, 720);
  }
  if (!ok) ok = usb_disp_set_auto_mode(d);
  if (ok && usb_disp_width(d) > 1920u) {
    // Line buffers are sized for 1920 (USB_DISP_MAX_WIDTH).
    ok = usb_disp_set_mode(d, 1280, 720);
  }
  if (!ok) return;  // retried on the next connection event

  const uint16_t w = usb_disp_width(d);
  const uint16_t h = usb_disp_height(d);
  if (w == 0 || h == 0) return;
  lcdtap::LcdTapConfig cfg = s->inst->getConfig();
  if (cfg.dviWidth != w || cfg.dviHeight != h) {
    cfg.dviWidth = w;
    cfg.dviHeight = h;
    s->inst->updateConfig(cfg);
  }
  s->outW = w;
  s->outH = h;
  s->modeSet = true;
  startFullRepaint(s);
  s->lastEpoch = s->inst->getPresentationEpoch();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool displaylinkOutInit(DisplaylinkOutState* s, lcdtap::LcdTap* inst,
                        DisplaylinkFillCallback fillCb, void* fillUser,
                        const DisplaylinkOutConfig& cfg) {
  memset(s, 0, sizeof(*s));
  s->inst = inst;
  s->fillCb = fillCb;
  s->fillUser = fillUser;
  s->cfg = cfg;
  s->aggCol = 0xFFFFFFFFu;

  uint16_t w, h;
  inst->getOutputScreenSize(&w, &h);
  s->outW = w;
  s->outH = h;

  usb_disp_init();
  usb_disp_config_t c = {};
  c.port = cfg.pioIndex;
  c.pin_dp = cfg.pinDp;
  c.pin_dm = cfg.pinDm;
  c.no_auto_mode = true;  // the pump sets the mode explicitly
  s->disp = usb_disp_add_cfg(&c);
  return s->disp != nullptr;
}

static DisplaylinkOutState* sCore1State = nullptr;

// SRAM-resident so the park busy-wait never touches flash. usb_disp_task()
// itself is flash code, but it only runs while not parked.
static void __not_in_flash_func(displaylinkCore1Main)() {
  DisplaylinkOutState* s = sCore1State;
  while (true) {
    if (s->parkReq) {
      s->parked = true;
      while (s->parkReq) tight_loop_contents();
      s->parked = false;
    }
    usb_disp_task();
  }
}

void displaylinkOutLaunchCore1(DisplaylinkOutState* s) {
  sCore1State = s;
  usb_disp_start_manual();
  multicore_launch_core1(displaylinkCore1Main);
}

bool displaylinkOutConsumeNewFrame(DisplaylinkOutState* s) {
  uint32_t now = nowMs();
  if (static_cast<int32_t>(now - s->nextFrameMs) < 0) return false;
  s->nextFrameMs = now + FRAME_TICK_MS;
  ++s->frameCount;
  if (s->cfg.ledToggleFrames != 0 &&
      s->frameCount % s->cfg.ledToggleFrames == 0) {
    gpio_xor_mask(1u << s->cfg.ledPin);
  }
  return true;
}

void displaylinkOutFlashAcquire(DisplaylinkOutState* s) {
  s->parkReq = true;
  while (!s->parked) tight_loop_contents();
}

void displaylinkOutFlashRelease(DisplaylinkOutState* s) {
  s->parkReq = false;
  while (s->parked) tight_loop_contents();
  // SOF stopped for the whole flash write (well past the 3 ms USB suspend
  // threshold), so the adapter has entered suspend. This host has no resume
  // signalling, restarting SOFs alone does not reliably wake DL chips, and a
  // suspended bus idles at J so no disconnect edge is ever detected — the
  // device would just stay dark. Recover deterministically instead: force a
  // re-enumeration (the detect path also flushes the bulk ring, so no
  // half-queued command reaches the fresh connection) and renegotiate the
  // mode, which ends in a full repaint.
  if (s->disp != nullptr) {
    usb_disp_force_reenum(D(s));
    s->modeSet = false;
  }
}

void displaylinkOutSetOsdRect(DisplaylinkOutState* s, bool active, uint16_t x,
                              uint16_t y, uint16_t w, uint16_t h) {
  const bool was = s->osdActive;
  s->osdActive = active;
  if (active) {
    s->osdX = x;
    s->osdY = y;
    s->osdW = w;
    s->osdH = h;
  }
  if (was && !active && s->osdW != 0) {
    // One final sweep to erase the overlay with framebuffer content.
    s->osdSweepActive = true;
    s->osdSweepY = s->osdY;
  }
}

void displaylinkOutProcess(DisplaylinkOutState* s) {
  usb_disp_t* d = D(s);
  if (!d) return;
  const uint32_t now = nowMs();

  // Hotplug / enumeration (blocking work happens only on connection events).
  if (static_cast<int32_t>(now - s->nextPollMs) >= 0) {
    s->nextPollMs = now + POLL_INTERVAL_MS;
    if (usb_disp_poll(d)) {
      s->modeSet = false;  // (re)connected or lost: renegotiate the mode
    }
  }
  if (!usb_disp_ready(d)) {
    s->modeSet = false;
    return;
  }
  if (!s->modeSet) {
    setupMode(s);
    if (!s->modeSet) return;
  }

  // Output changed without fb writes (inversion, sleep, scaling, reset...).
  const uint32_t epoch = s->inst->getPresentationEpoch();
  if (epoch != s->lastEpoch) {
    s->lastEpoch = epoch;
    startFullRepaint(s);
  }

  lcdtap::OutputMapInfo mi;
  s->inst->getOutputMapInfo(&mi);

  uint32_t fills = 0;
  // Finish any duplicate-line group left over from the previous call first;
  // its dirty bits are already claimed. lineBuf does not survive between
  // calls (OSD sweeps and other groups reuse it), so schedule a re-fill.
  if (s->groupActive) {
    s->groupNeedFill = !s->groupPerLine;
    fills += pumpPendingGroup(s, MAX_FILLS_PER_CALL);
  }
  if (!s->groupActive) {
    if (s->fullRepaint) {
      while (fills < MAX_FILLS_PER_CALL && s->fullRepaint && !s->groupActive) {
        uint32_t n = sweepStep(s, mi, MAX_FILLS_PER_CALL - fills);
        if (n == 0) break;  // ring full
        fills += n;
      }
    } else if ((mi.rotation & 1u) == 0) {
      fills += dirtyStepRot02(s, mi, MAX_FILLS_PER_CALL - fills);
    } else {
      fills += dirtyStepRot13(s, mi, MAX_FILLS_PER_CALL - fills);
    }
  }

  // Periodic OSD overlay refresh while the menu is open.
  if (s->osdActive && !s->osdSweepActive &&
      static_cast<int32_t>(now - s->nextOsdMs) >= 0) {
    s->osdSweepActive = true;
    s->osdSweepY = s->osdY;
  }
  if (!s->groupActive && fills < MAX_FILLS_PER_CALL) {
    osdStep(s, MAX_FILLS_PER_CALL - fills);
  }

  // A send failed after its dirty bits were already claimed (transient NAK
  // stall, device suspend/drop). Repaint everything once the link recovers
  // rather than leaving stale regions the value compare would never re-mark.
  if (s->sendFailed) {
    s->sendFailed = false;
    startFullRepaint(s);
  }

  // Flush the stream after every burst. Two layers hold back the tail of the
  // last command otherwise: the USB host only transmits full 64-byte packets
  // (the sub-packet residue waits in the ring for more data), and the DL
  // chip itself defers the final command until the next data arrives (see
  // dl_flush's 0xAF 0xA0 sync command). Without this, up to ~63 bytes — an
  // entire small RLE line command, or the tail of a span — stay unapplied
  // for as long as the screen is quiet, which shows up as thin stale lines
  // at update-span edges (= 64px segment boundaries). timeout 0 makes this
  // fire-and-forget: the flush request is latched and serviced by Core 1.
  if (s->needFlush) {
    s->needFlush = false;
    usb_disp_flush(d, 0);
  }
}
