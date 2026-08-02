#ifndef LCDTAP_PICO2_SPLASH_HPP
#define LCDTAP_PICO2_SPLASH_HPP

#include <stdint.h>

namespace lcdtap::pico2 {

// Fills an RGB565 framebuffer with a splash image so that it appears
// upright and centered on screen for any outputRotation (0/90/180/270).
// Any margin outside the image area is cleared to black, so this can be
// used directly on framebuffers larger than the image too. The source
// pixels are big-endian RGB565 byte pairs, the same layout RAMWR data is
// stored in, so they are copied as (hi << 8) | lo.
void drawSplash(uint16_t *fb, uint16_t buffWidth, uint16_t buffHeight,
                uint8_t outputRotation, const uint8_t *image,
                uint16_t imageWidth, uint16_t imageHeight);

}  // namespace lcdtap::pico2

#endif
