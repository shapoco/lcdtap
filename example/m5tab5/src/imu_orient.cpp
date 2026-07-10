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

// Empirically determined on hardware: the raw device accel axes map to
// panel orientation one quadrant short of the convention documented in
// imu_orient.hpp (real tilt data was only available once a BMI270
// bring-up issue was worked around, so this mapping had never been
// validated against actual tilt before). Originally corrected by
// advancing the quantized quadrant by one 90° CW step (value 1).
//
// main.cpp's switch from M5.Display.setRotation(3) to setRotation(0)
// (panel's native physical orientation, see project perf-tuning notes)
// shifted the panel's own presentation by one quadrant, which showed up
// as OSD/keypad content rendering rotated 90° CW from correct. Since
// this correction already exists purely to align the quantized IMU
// quadrant with the panel's presentation, undoing exactly one CW step
// (1 -> 0) cancels it out -- confirmed against the observed symptom
// (content tilted CW by exactly one quadrant, not a mirrored/broken
// pattern), so no other rotation math needed to change.
static constexpr uint8_t ORIENT_CORRECTION = 2;

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
  // pulls that way (verified on hardware; see ORIENT_CORRECTION below
  // for the residual quadrant offset this mapping still needs).
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
  cand = (uint8_t)((cand + ORIENT_CORRECTION) & 3u);

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
