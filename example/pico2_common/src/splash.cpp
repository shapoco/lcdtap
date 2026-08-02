#include "lcdtap/pico2/splash.hpp"

namespace lcdtap::pico2 {

// fillScanline() applies outputRotation as a true geometric rotation of
// the framebuffer content: rot=1 shows the framebuffer rotated 90 degrees
// CW, rot=3 90 degrees CCW, rot=2 point-mirrored. Each framebuffer pixel
// is therefore mapped to its post-rotation screen position and the splash
// is sampled there, which pre-applies the inverse rotation.
void drawSplash(uint16_t *fb, uint16_t buffWidth, uint16_t buffHeight,
                uint8_t outputRotation, const uint8_t *image,
                uint16_t imageWidth, uint16_t imageHeight) {
  const uint8_t rot = outputRotation & 3u;
  const uint16_t screenW = (rot & 1u) ? buffHeight : buffWidth;
  const uint16_t screenH = (rot & 1u) ? buffWidth : buffHeight;
  const int16_t offX = ((int16_t)screenW - (int16_t)imageWidth) / 2;
  const int16_t offY = ((int16_t)screenH - (int16_t)imageHeight) / 2;
  for (uint16_t y = 0; y < buffHeight; ++y) {
    uint16_t *row = fb + (uint32_t)y * buffWidth;
    for (uint16_t x = 0; x < buffWidth; ++x) {
      // Screen-space position (sx, sy) of framebuffer pixel (x, y) after
      // fillScanline() applies outputRotation.
      int16_t sx, sy;
      switch (rot) {
        default:
        case 0:
          sx = (int16_t)x;
          sy = (int16_t)y;
          break;
        case 1:
          sx = (int16_t)(buffHeight - 1 - y);
          sy = (int16_t)x;
          break;
        case 2:
          sx = (int16_t)(buffWidth - 1 - x);
          sy = (int16_t)(buffHeight - 1 - y);
          break;
        case 3:
          sx = (int16_t)y;
          sy = (int16_t)(buffWidth - 1 - x);
          break;
      }
      sx -= offX;
      sy -= offY;
      uint16_t px = 0x0000u;
      if (sx >= 0 && sx < (int16_t)imageWidth && sy >= 0 &&
          sy < (int16_t)imageHeight) {
        const uint8_t *p = &image[((uint32_t)sy * imageWidth + sx) * 2u];
        px = (uint16_t)((p[0] << 8) | p[1]);
      }
      row[x] = px;
    }
  }
}

}  // namespace lcdtap::pico2
