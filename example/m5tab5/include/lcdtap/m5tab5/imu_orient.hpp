#pragma once
#include <cstdint>

namespace lcdtap::m5tab5 {

// Quantizes the gravity vector from the IMU into one of 4 screen-space
// orientations so the virtual keypad can follow the user's point of view.
//
// orient semantics (rotation of "user's down edge" relative to the panel):
//   0: user's down = panel bottom edge (normal landscape)
//   1: user's down = panel right edge  (panel rotated 90° CW from user view)
//   2: user's down = panel top edge    (upside down)
//   3: user's down = panel left edge

struct ImuOrientState {
  float fx, fy, fz;  // low-pass filtered accel
  bool filterInit;
  uint8_t orient;
  uint8_t candidate;
  uint64_t candidateSinceMs;
};

void imuOrientInit(ImuOrientState *s);

// Feed one accel sample (device coordinates, in g).
// Returns true when a stable orientation change commits.
bool imuOrientUpdate(ImuOrientState *s, uint64_t nowMs, float ax, float ay,
                     float az);

static inline uint8_t imuOrientGet(const ImuOrientState *s) {
  return s->orient;
}

}  // namespace lcdtap::m5tab5
