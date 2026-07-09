// Gravity-based orientation detection for the virtual keypad.
//
// The accel vector is low-pass filtered, projected onto the panel plane,
// and quantized into 4 orientations. A candidate orientation must stay
// within ±30° of an axis (i.e. 15° past the 45° quadrant boundary) for
// ORIENT_HOLD_MS before it commits, which gives both angular hysteresis
// and temporal debouncing. When the device lies flat (in-plane gravity
// below threshold) the previous orientation is kept.

#include "lcdtap/m5tab5/imu_orient.hpp"

namespace lcdtap::m5tab5 {

static constexpr float FILTER_ALPHA = 0.2f;
static constexpr float MIN_IN_PLANE_G = 0.5f;
// |perp| < |along| * tan(30°) → within 30° of the candidate axis
static constexpr float TAN_30_DEG = 0.5774f;
static constexpr uint64_t ORIENT_HOLD_MS = 300;

void imuOrientInit(ImuOrientState *s) {
  s->fx = s->fy = s->fz = 0.0f;
  s->filterInit = false;
  s->orient = 0;
  s->candidate = 0;
  s->candidateSinceMs = 0;
}

bool imuOrientUpdate(ImuOrientState *s, uint64_t nowMs, float ax, float ay,
                     float az) {
  if (!s->filterInit) {
    s->fx = ax;
    s->fy = ay;
    s->fz = az;
    s->filterInit = true;
  } else {
    s->fx += FILTER_ALPHA * (ax - s->fx);
    s->fy += FILTER_ALPHA * (ay - s->fy);
    s->fz += FILTER_ALPHA * (az - s->fz);
  }

  // Map device accel axes to panel coordinates. gx points toward the
  // panel's +X (right) edge, gy toward +Y (bottom) edge when gravity
  // pulls that way. Signs flipped together with the display rotation
  // change (1 -> 3); still to be verified on hardware — adjust
  // signs/swaps here if the keypad appears on a wrong edge.
  float gx = -s->fx;
  float gy = -s->fy;

  if (gx * gx + gy * gy < MIN_IN_PLANE_G * MIN_IN_PLANE_G) {
    s->candidate = s->orient;
    return false;  // lying flat; keep the previous orientation
  }

  // Determine which panel edge gravity points to, with ±30° window.
  float absX = (gx < 0) ? -gx : gx;
  float absY = (gy < 0) ? -gy : gy;
  uint8_t cand;
  if (absY > absX) {
    if (absX > absY * TAN_30_DEG) return false;  // near a quadrant boundary
    cand = (gy > 0) ? 0 : 2;                     // bottom : top
  } else {
    if (absY > absX * TAN_30_DEG) return false;
    cand = (gx > 0) ? 1 : 3;  // right : left
  }

  if (cand == s->orient) {
    s->candidate = cand;
    return false;
  }
  if (cand != s->candidate) {
    s->candidate = cand;
    s->candidateSinceMs = nowMs;
    return false;
  }
  if (nowMs - s->candidateSinceMs < ORIENT_HOLD_MS) return false;

  s->orient = cand;
  return true;
}

}  // namespace lcdtap::m5tab5
