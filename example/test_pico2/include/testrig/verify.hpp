#pragma once

// Streaming framebuffer verifier (platform independent, host-testable).
//
// Consumes the Base64 payload of a getframebuffer response character by
// character (base64 -> RGB565-RLE -> pixels) and compares each pixel on the
// fly against the counter-hash pattern, so no readback buffer is needed.
//
// The target emits the trim region in post-rotation output order
// (fbIndexTrimmed in pico2_common json_intf.cpp); the verifier applies the
// same mapping to recover physical coordinates for the expected value.

#include <cstdint>

#include "testrig/testgen.hpp"

namespace testrig {

struct VerifyParams {
  PatternParams pattern;
  uint8_t rot;  // target outputRot (0..3)
  bool rbSwap;  // target cachedBGR (see expectedRgb565)
  // Readback source region (trim region) in physical coordinates. For
  // trim-off vectors this is the full framebuffer.
  uint16_t srcX, srcY, srcW, srcH;
};

class FrameVerifier {
 public:
  void begin(const VerifyParams& p);

  // Post-rotation dimensions the response header must report.
  uint16_t expectedWidth() const { return outW_; }
  uint16_t expectedHeight() const { return outH_; }

  // Feed one character of the Base64 "data" value ('=' padding included,
  // closing quote excluded).
  void feedBase64(char c);

  // Returns true when exactly outW*outH pixels arrived, all matched, and the
  // stream had no structural error.
  bool finish();

  uint32_t pixelCount() const { return pixelCount_; }
  uint32_t mismatchCount() const { return mismatchCount_; }
  bool streamError() const { return streamError_; }
  // First mismatch, physical coordinates and masked values.
  uint16_t firstBadX() const { return firstBadX_; }
  uint16_t firstBadY() const { return firstBadY_; }
  uint16_t firstGot() const { return firstGot_; }
  uint16_t firstWant() const { return firstWant_; }

 private:
  void feedByte(uint8_t b);
  void feedPixel(uint16_t px);

  VerifyParams p_;
  uint16_t mask_;
  uint16_t outW_, outH_;
  uint16_t outX_, outY_;

  // Base64 accumulator
  uint32_t b64Acc_;
  uint8_t b64Count_;
  uint8_t b64Pad_;

  // RLE state
  enum class RleState : uint8_t { CONTROL, RUN_LO, RUN_HI, LIT_LO, LIT_HI };
  RleState rleState_;
  uint8_t rleCount_;  // remaining pixels of the current packet
  uint8_t pxLo_;

  uint32_t pixelCount_;
  uint32_t mismatchCount_;
  bool streamError_;
  uint16_t firstBadX_, firstBadY_, firstGot_, firstWant_;
};

// Compare a gettextbuffer payload (row-major character codes) against the
// deterministic text pattern. Returns the number of mismatching cells.
uint32_t textMismatchCount(uint32_t seed, uint16_t cols, uint16_t rows,
                           const uint8_t* data);

}  // namespace testrig
