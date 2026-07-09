// Virtual touch keypad rendering and input handling.
//
// Icons are 8-bit alpha masks generated at init by 4x4-supersampled
// coverage tests (circle ring + glyph), blended into the RGB565 strip as
// semi-transparent white with a translucent black drop shadow offset by
// (+2, +2) px so the icons stay visible on white content.
//
// Key roles are laid out in the user's frame (Enter center, directions
// around it) and transformed to panel coordinates by the IMU orientation.
// Direction keys are remapped so that the arrow that visually points to
// the panel's top edge sends OSD_KEY_UP — the OSD raster is always drawn
// in fixed panel orientation.

#include "lcdtap/m5tab5/keypad.hpp"

#include "lcdtap/osd.hpp"

namespace lcdtap::m5tab5 {

// Peak icon opacity (semi-transparent white), 0..255.
static constexpr int FILL_OPACITY = 192;
// Shadow opacity relative to the icon coverage, 0..255.
static constexpr int SHADOW_OPACITY = 112;
static constexpr int SHADOW_OFFSET = 2;

// Idle time before the fade-out starts, and the fade duration.
static constexpr uint64_t KEYPAD_IDLE_HIDE_MS = 5000;
static constexpr uint64_t KEYPAD_FADE_MS = 2000;

// Key roles (index into layout tables).
enum : uint8_t {
  KEY_UP = 0,
  KEY_DOWN = 1,
  KEY_LEFT = 2,
  KEY_RIGHT = 3,
  KEY_ENTER = 4,
  NUM_KEYS = 5,
};

// User-frame offsets of each key from the cluster center, in pitch units.
static constexpr int8_t KEY_OFS_X[NUM_KEYS] = {0, 0, -1, 1, 0};
static constexpr int8_t KEY_OFS_Y[NUM_KEYS] = {-1, 1, 0, 0, 0};
// Glyph rotation of the "up" arrow for each direction key (90° CW steps).
static constexpr uint8_t KEY_GLYPH_ROT[NUM_KEYS] = {0, 2, 3, 1, 0};

//=============================================================================
// Icon mask generation (init-time, 4x4 supersampling)
//=============================================================================

// All coverage tests work in unit coordinates [-1, +1].

static bool inRing(float x, float y) {
  float r2 = x * x + y * y;
  return r2 >= 0.80f * 0.80f && r2 <= 0.94f * 0.94f;
}

static bool inTriangle(float x, float y, float x0, float y0, float x1, float y1,
                       float x2, float y2) {
  // Half-plane edge functions; vertices given counter-clockwise.
  float e0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
  float e1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
  float e2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
  return (e0 <= 0 && e1 <= 0 && e2 <= 0) || (e0 >= 0 && e1 >= 0 && e2 >= 0);
}

// Filled triangle pointing up, enclosed by the ring.
static bool arrowGlyph(float x, float y) {
  return inTriangle(x, y, 0.0f, -0.52f, -0.45f, 0.36f, 0.45f, 0.36f);
}

// Bent "return" arrow: down stroke, left stroke, arrowhead at the left end.
static bool enterGlyph(float x, float y) {
  constexpr float T = 0.13f;  // stroke half thickness
  // Vertical stroke: from top right, down to the corner.
  if (x >= 0.30f - T && x <= 0.30f + T && y >= -0.48f && y <= 0.18f + T) {
    return true;
  }
  // Horizontal stroke: from the corner to the arrowhead.
  if (y >= 0.18f - T && y <= 0.18f + T && x >= -0.16f && x <= 0.30f + T) {
    return true;
  }
  // Arrowhead pointing left.
  return inTriangle(x, y, -0.52f, 0.18f, -0.16f, -0.06f, -0.16f, 0.42f);
}

static void generateMask(uint8_t *mask, bool (*glyph)(float, float)) {
  constexpr int N = KEYPAD_ICON_SIZE;
  constexpr int SS = 4;
  for (int py = 0; py < N; ++py) {
    for (int px = 0; px < N; ++px) {
      int hit = 0;
      for (int sy = 0; sy < SS; ++sy) {
        for (int sx = 0; sx < SS; ++sx) {
          float x = ((px + (sx + 0.5f) / SS) * 2.0f / N) - 1.0f;
          float y = ((py + (sy + 0.5f) / SS) * 2.0f / N) - 1.0f;
          if (inRing(x, y) || glyph(x, y)) ++hit;
        }
      }
      mask[py * N + px] = (uint8_t)((hit * FILL_OPACITY) / (SS * SS));
    }
  }
}

//=============================================================================
// Geometry helpers
//=============================================================================

// Rotate user-frame coordinates to panel coordinates (orient = which panel
// edge is at the user's bottom).
static void userToPanel(uint8_t orient, int ux, int uy, int *px, int *py) {
  switch (orient & 3u) {
    default:
    case 0:
      *px = ux;
      *py = uy;
      break;
    case 1:
      *px = uy;
      *py = -ux;
      break;
    case 2:
      *px = -ux;
      *py = -uy;
      break;
    case 3:
      *px = -uy;
      *py = ux;
      break;
  }
}

// Panel-space center of the keypad cluster for the given orientation:
// bottom-center of the screen in the user's frame.
static void clusterCenter(const KeypadState *s, uint8_t orient, int *cx,
                          int *cy) {
  // Distance from the user's bottom screen edge to the cluster center.
  int inset = KEYPAD_EDGE_MARGIN + KEYPAD_ICON_SIZE / 2 + KEYPAD_KEY_PITCH;
  switch (orient & 3u) {
    default:
    case 0:
      *cx = s->screenW / 2;
      *cy = s->screenH - inset;
      break;
    case 1:
      *cx = s->screenW - inset;
      *cy = s->screenH / 2;
      break;
    case 2:
      *cx = s->screenW / 2;
      *cy = inset;
      break;
    case 3:
      *cx = inset;
      *cy = s->screenH / 2;
      break;
  }
}

static void keyCenter(const KeypadState *s, uint8_t orient, int key, int *px,
                      int *py) {
  int cx, cy;
  clusterCenter(s, orient, &cx, &cy);
  int dx, dy;
  userToPanel(orient, KEY_OFS_X[key] * KEYPAD_KEY_PITCH,
              KEY_OFS_Y[key] * KEYPAD_KEY_PITCH, &dx, &dy);
  *px = cx + dx;
  *py = cy + dy;
}

// Sample a mask rotated by `rot` 90°-CW steps at (x, y); returns 0 outside.
static inline uint8_t sampleMask(const uint8_t *mask, int rot, int x, int y) {
  constexpr int N = KEYPAD_ICON_SIZE;
  if ((unsigned)x >= (unsigned)N || (unsigned)y >= (unsigned)N) return 0;
  switch (rot & 3) {
    default:
    case 0: return mask[y * N + x];
    case 1: return mask[(N - 1 - x) * N + y];
    case 2: return mask[(N - 1 - y) * N + (N - 1 - x)];
    case 3: return mask[x * N + (N - 1 - y)];
  }
}

// Blend src into dst with 5-bit alpha (0..32) using the packed
// 0x07E0F81F RGB565 trick (integer only).
static inline uint16_t blend565(uint16_t dst, uint16_t src, uint32_t alpha5) {
  uint32_t d = ((uint32_t)dst | ((uint32_t)dst << 16)) & 0x07E0F81Fu;
  uint32_t v = ((uint32_t)src | ((uint32_t)src << 16)) & 0x07E0F81Fu;
  uint32_t r = ((((v - d) * alpha5) >> 5) + d) & 0x07E0F81Fu;
  return (uint16_t)(r | (r >> 16));
}

//=============================================================================
// Public API
//=============================================================================

void keypadInit(KeypadState *s, uint16_t screenW, uint16_t screenH) {
  s->screenW = screenW;
  s->screenH = screenH;
  s->visible = false;
  s->summonProtect = false;
  s->globalAlpha = 0;
  s->lastActivityMs = 0;
  s->orient = 0;
  s->pressedMask = 0;
  generateMask(s->maskArrow, arrowGlyph);
  generateMask(s->maskEnter, enterGlyph);
}

void keypadNotifyActivity(KeypadState *s, uint64_t nowMs) {
  s->visible = true;
  s->globalAlpha = 255;
  s->lastActivityMs = nowMs;
}

// Map a pressed key role to the OSD key bit, compensating the IMU
// orientation so arrows act in panel (= OSD raster) space.
static uint8_t roleToOsdKey(uint8_t role, uint8_t orient) {
  if (role == KEY_ENTER) return lcdtap::OSD_KEY_ENTER;
  // Clockwise direction cycle: UP → RIGHT → DOWN → LEFT.
  static constexpr uint8_t CW_OF_ROLE[4] = {0, 2, 3, 1};  // UP,DOWN,LEFT,RIGHT
  static constexpr uint8_t OSD_OF_CW[4] = {
      lcdtap::OSD_KEY_UP, lcdtap::OSD_KEY_RIGHT, lcdtap::OSD_KEY_DOWN,
      lcdtap::OSD_KEY_LEFT};
  uint8_t cw = (uint8_t)((CW_OF_ROLE[role] - orient) & 3u);
  return OSD_OF_CW[cw];
}

uint8_t keypadUpdate(KeypadState *s, uint64_t nowMs, const KeypadTouch *pts,
                     int numPts, uint8_t orient) {
  if (orient != s->orient) {
    s->orient = orient;
    keypadNotifyActivity(s, nowMs);
  }

  bool touched = numPts > 0;

  if (!s->visible) {
    s->pressedMask = 0;
    if (touched) {
      keypadNotifyActivity(s, nowMs);
      // The summoning touch must not generate key events.
      s->summonProtect = true;
    }
    return 0;
  }

  if (touched) {
    keypadNotifyActivity(s, nowMs);
  } else {
    s->summonProtect = false;
  }

  // Fade-out state machine.
  uint64_t idle = nowMs - s->lastActivityMs;
  if (idle >= KEYPAD_IDLE_HIDE_MS + KEYPAD_FADE_MS) {
    s->visible = false;
    s->globalAlpha = 0;
    s->pressedMask = 0;
    return 0;
  } else if (idle >= KEYPAD_IDLE_HIDE_MS) {
    uint32_t t = (uint32_t)(idle - KEYPAD_IDLE_HIDE_MS);
    s->globalAlpha = (uint8_t)(255u - (255u * t) / KEYPAD_FADE_MS);
  } else {
    s->globalAlpha = 255;
  }

  // Hit-test touches against the key circles.
  uint8_t pressed = 0;
  if (!s->summonProtect) {
    constexpr int R = KEYPAD_ICON_SIZE / 2;
    for (int k = 0; k < NUM_KEYS; ++k) {
      int kx, ky;
      keyCenter(s, s->orient, k, &kx, &ky);
      for (int i = 0; i < numPts; ++i) {
        int dx = pts[i].x - kx;
        int dy = pts[i].y - ky;
        if (dx * dx + dy * dy <= R * R) {
          pressed |= (uint8_t)(1u << k);
          break;
        }
      }
    }
  }
  s->pressedMask = pressed;

  uint8_t osdKeys = 0;
  for (int k = 0; k < NUM_KEYS; ++k) {
    if (pressed & (1u << k)) osdKeys |= roleToOsdKey((uint8_t)k, s->orient);
  }
  return osdKeys;
}

void keypadFillStrip(const KeypadState *s, uint16_t yTop, uint16_t numLines,
                     uint16_t *strip, uint16_t stripWidth) {
  if (!s->visible || s->globalAlpha == 0) return;
  constexpr int N = KEYPAD_ICON_SIZE;

  for (int k = 0; k < NUM_KEYS; ++k) {
    int kx, ky;
    keyCenter(s, s->orient, k, &kx, &ky);
    int left = kx - N / 2;
    int top = ky - N / 2;

    // Intersect the icon bounding box (expanded by the shadow offset)
    // with this strip.
    int y0 = top;
    int y1 = top + N + SHADOW_OFFSET;
    if (y0 < yTop) y0 = yTop;
    if (y1 > yTop + numLines) y1 = yTop + numLines;
    if (y0 >= y1) continue;

    const uint8_t *mask = (k == KEY_ENTER) ? s->maskEnter : s->maskArrow;
    int rot = (KEY_GLYPH_ROT[k] + s->orient) & 3;

    // The mask already encodes FILL_OPACITY at its peak; the pressed key
    // is boosted toward full opacity for visual feedback.
    uint32_t ga = s->globalAlpha;
    uint32_t shadowA = (ga * SHADOW_OPACITY) / 255;
    bool isPressed = (s->pressedMask & (1u << k)) != 0;

    for (int y = y0; y < y1; ++y) {
      uint16_t *row = strip + (uint32_t)(y - yTop) * stripWidth;
      int x0 = left < 0 ? 0 : left;
      int x1 = left + N + SHADOW_OFFSET;
      if (x1 > s->screenW) x1 = s->screenW;
      for (int x = x0; x < x1; ++x) {
        int lx = x - left;
        int ly = y - top;
        // Shadow: icon mask sampled at (-offset, -offset).
        uint8_t sm =
            sampleMask(mask, rot, lx - SHADOW_OFFSET, ly - SHADOW_OFFSET);
        uint16_t px = row[x];
        if (sm) {
          uint32_t a5 = ((uint32_t)sm * shadowA) >> 11;  // 0..~27
          if (a5) px = blend565(px, 0x0000, a5);
        }
        uint8_t fm = sampleMask(mask, rot, lx, ly);
        if (fm) {
          uint32_t fa = (uint32_t)fm * ga;
          if (isPressed) fa = (fa * 255u) / FILL_OPACITY;
          uint32_t a5 = fa >> 11;
          if (a5 > 32) a5 = 32;
          if (a5) px = blend565(px, 0xFFFF, a5);
        }
        row[x] = px;
      }
    }
  }
}

}  // namespace lcdtap::m5tab5
