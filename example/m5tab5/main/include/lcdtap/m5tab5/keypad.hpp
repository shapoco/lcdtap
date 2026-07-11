#pragma once
#include <cstdint>

namespace lcdtap::m5tab5 {

// Virtual touch keypad: an Enter key surrounded by four direction keys,
// overlaid semi-transparently at the bottom-center of the screen from the
// user's point of view (IMU orientation). Hidden by default; summoned by
// a touch or an orientation change, fades out after idle.

static constexpr int KEYPAD_ICON_SIZE = 96;    // alpha mask dimension (px)
static constexpr int KEYPAD_KEY_PITCH = 112;   // center distance between keys
static constexpr int KEYPAD_EDGE_MARGIN = 24;  // gap to the user's bottom edge

struct KeypadTouch {
  int16_t x, y;  // panel coordinates
};

struct KeypadState {
  uint16_t screenW, screenH;
  bool visible;
  bool summonProtect;   // suppress key events until the summoning touch ends
  uint8_t globalAlpha;  // 255 while active, ramps to 0 during fade-out
  uint64_t lastActivityMs;
  uint8_t orient;       // panel edge at the user's bottom (0..3)
  uint8_t pressedMask;  // raw role bits (bit i = key i pressed), no remap
  // Precomputed 8-bit alpha masks (glyph + enclosing ring), generated at init.
  uint8_t maskArrow[KEYPAD_ICON_SIZE * KEYPAD_ICON_SIZE];
  uint8_t maskEnter[KEYPAD_ICON_SIZE * KEYPAD_ICON_SIZE];
};

void keypadInit(KeypadState *s, uint16_t screenW, uint16_t screenH);

// Notify non-touch activity (e.g. committed orientation change): summons
// the keypad and resets the idle timer.
void keypadNotifyActivity(KeypadState *s, uint64_t nowMs);

// Call once per frame with the current touch points.
// Returns the OSD_KEY_* bitmask. Direction keys map to their user-view
// roles directly (no rotation compensation): the OSD is composited with
// the same orientation as the keypad, so user-up is always menu-up.
uint8_t keypadUpdate(KeypadState *s, uint64_t nowMs, const KeypadTouch *pts,
                     int numPts, uint8_t orient);

// Alpha-blend the keypad into strip rows [yTop, yTop + numLines).
void keypadFillStrip(const KeypadState *s, uint16_t yTop, uint16_t numLines,
                     uint16_t *strip, uint16_t stripWidth);

}  // namespace lcdtap::m5tab5
