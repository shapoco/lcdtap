#pragma once

#include <cstdint>

#include "pico/stdlib.h"

#include "lcdtap/lcdtap.hpp"

// DisplayLink (USB Full-Speed, DL-1x0/1x5) output backend.
//
// Unlike the HSTX/composite backends there is no scanout deadline: the
// adapter refreshes the monitor at 60 Hz from its own framebuffer and the
// pump on Core 0 sends best-effort partial updates over a ~1 MB/s bulk
// stream. Core 1 is dedicated to the PIO USB host service loop
// (usb_disp_task) with a cooperative park for flash writes.

// Overlay callback, same shape as the HSTX/composite fill callback: invoked
// after LcdTap::fillScanline for each generated output line.
using DisplaylinkFillCallback = void (*)(uint16_t scanY, uint16_t* buf,
                                         void* userData);

struct DisplaylinkOutConfig {
  uint ledPin;
  uint32_t ledToggleFrames;  // LED toggles every N frame ticks (~33ms each)
  uint8_t pioIndex;          // dedicated PIO block number (0..2)
  uint8_t pinDp;             // D+ GPIO (must be adjacent to pinDm)
  uint8_t pinDm;             // D- GPIO
  uint16_t width;            // target mode; falls back to 1280x720 when the
  uint16_t height;           // chip rejects it (e.g. 1080p on a DL-125)
};

struct DisplaylinkOutState {
  lcdtap::LcdTap* inst;
  DisplaylinkFillCallback fillCb;
  void* fillUser;
  void* disp;  // usb_disp_t* (kept opaque so main.cpp needs no usb_disp.h)
  DisplaylinkOutConfig cfg;

  // Connection / mode
  bool modeSet;
  uint16_t outW, outH;  // active output mode (= LcdTap dviWidth/dviHeight)

  // Pump state
  uint32_t lastEpoch;
  bool sendFailed;     // a bulk send failed; triggers a full repaint
  bool fullRepaint;    // resend everything incl. borders (epoch/connect)
  uint32_t sweepY;     // full-repaint cursor (output line)
  uint16_t dirtyRow;   // incremental cursor (fb row, rot=0/2)
  bool aggValid;       // rot=1/3: per-segment aggregates are claimed
  uint8_t aggPending;  // rot=1/3: segment bits still to process
  uint8_t aggSeg;      // rot=1/3: current segment
  uint32_t aggCol;     // rot=1/3: current fb column within the segment
  uint16_t aggRowMin[8], aggRowMax[8];
  uint32_t nextPollMs;

  // OSD overlay resend (output coordinates; independent of fb dirt)
  bool osdActive;
  bool osdSweepActive;
  uint16_t osdX, osdY, osdW, osdH;
  uint32_t osdSweepY;
  uint32_t nextOsdMs;

  // Frame tick (~33ms) for OSD key handling and LED blink on Core 0
  uint32_t nextFrameMs;
  bool newFrame;
  uint32_t frameCount;

  // Cooperative flash park handshake with the Core 1 service loop
  volatile bool parkReq;
  volatile bool parked;

  uint16_t lineBuf[1920];
};

// Register the USB display port. Call before displaylinkOutLaunchCore1().
bool displaylinkOutInit(DisplaylinkOutState* s, lcdtap::LcdTap* inst,
                        DisplaylinkFillCallback fillCb, void* fillUser,
                        const DisplaylinkOutConfig& cfg);

// Start the USB host service loop on Core 1 (manual-service mode).
void displaylinkOutLaunchCore1(DisplaylinkOutState* s);

// ~33ms tick; also drives the LED. Same contract as the other backends'
// consumeNewFrame: returns true once per tick.
bool displaylinkOutConsumeNewFrame(DisplaylinkOutState* s);

// Park/resume Core 1 in an SRAM busy-wait around flash writes. SOF stops
// while parked; the library's dead-bus detection re-enumerates the adapter
// if it does not survive the gap.
void displaylinkOutFlashAcquire(DisplaylinkOutState* s);
void displaylinkOutFlashRelease(DisplaylinkOutState* s);

// Best-effort update pump; call every main-loop iteration on Core 0.
// Handles hotplug polling, mode setup, dirty-region sends and OSD refresh.
// Each call does a small bounded amount of work so input processing stays
// responsive.
void displaylinkOutProcess(DisplaylinkOutState* s);

// Output-coordinate rectangle of the OSD overlay. While active the rect is
// resent periodically (the OSD is drawn onto the generated lines, not into
// the LCD framebuffer); on deactivation it is resent once to erase it.
void displaylinkOutSetOsdRect(DisplaylinkOutState* s, bool active, uint16_t x,
                              uint16_t y, uint16_t w, uint16_t h);
