#include "testrig/verify.hpp"

namespace testrig {

void FrameVerifier::begin(const VerifyParams& p) {
  p_ = p;
  mask_ = compareMask(p.pattern.format, p.rbSwap);
  if ((p.rot & 1u) == 0u) {
    outW_ = p.srcW;
    outH_ = p.srcH;
  } else {
    outW_ = p.srcH;
    outH_ = p.srcW;
  }
  outX_ = 0;
  outY_ = 0;
  b64Acc_ = 0;
  b64Count_ = 0;
  b64Pad_ = 0;
  rleState_ = RleState::CONTROL;
  rleCount_ = 0;
  pxLo_ = 0;
  pixelCount_ = 0;
  mismatchCount_ = 0;
  streamError_ = false;
  firstBadX_ = firstBadY_ = firstGot_ = firstWant_ = 0;
}

void FrameVerifier::feedBase64(char c) {
  int v;
  if (c >= 'A' && c <= 'Z') {
    v = c - 'A';
  } else if (c >= 'a' && c <= 'z') {
    v = c - 'a' + 26;
  } else if (c >= '0' && c <= '9') {
    v = c - '0' + 52;
  } else if (c == '+') {
    v = 62;
  } else if (c == '/') {
    v = 63;
  } else if (c == '=') {
    // Padding counts as a zero digit; the pad count decides how many of the
    // three accumulated bytes are real.
    if (b64Pad_ >= 2u) {
      streamError_ = true;
      return;
    }
    b64Pad_++;
    v = 0;
  } else {
    streamError_ = true;
    return;
  }
  if (b64Pad_ != 0u && c != '=') {
    streamError_ = true;  // data after padding
    return;
  }
  b64Acc_ = (b64Acc_ << 6) | static_cast<uint32_t>(v);
  if (++b64Count_ == 4u) {
    feedByte(static_cast<uint8_t>(b64Acc_ >> 16));
    if (b64Pad_ < 2u) feedByte(static_cast<uint8_t>(b64Acc_ >> 8));
    if (b64Pad_ < 1u) feedByte(static_cast<uint8_t>(b64Acc_));
    b64Acc_ = 0;
    b64Count_ = 0;
  }
}

void FrameVerifier::feedByte(uint8_t b) {
  // Bytes past the expected pixel count are tolerated only as base64 slack;
  // real extra packets are caught by the pixel counter in feedPixel().
  switch (rleState_) {
    case RleState::CONTROL:
      rleCount_ = static_cast<uint8_t>((b & 0x7Fu) + 1u);
      rleState_ = (b & 0x80u) ? RleState::RUN_LO : RleState::LIT_LO;
      break;
    case RleState::RUN_LO:
      pxLo_ = b;
      rleState_ = RleState::RUN_HI;
      break;
    case RleState::RUN_HI: {
      uint16_t px = static_cast<uint16_t>(pxLo_ | (b << 8));
      for (uint8_t i = 0; i < rleCount_; i++) feedPixel(px);
      rleState_ = RleState::CONTROL;
      break;
    }
    case RleState::LIT_LO:
      pxLo_ = b;
      rleState_ = RleState::LIT_HI;
      break;
    case RleState::LIT_HI:
      feedPixel(static_cast<uint16_t>(pxLo_ | (b << 8)));
      rleState_ = (--rleCount_ == 0u) ? RleState::CONTROL : RleState::LIT_LO;
      break;
  }
}

void FrameVerifier::feedPixel(uint16_t px) {
  if (pixelCount_ >= static_cast<uint32_t>(outW_) * outH_) {
    streamError_ = true;
    return;
  }
  // Inverse of the target's fbIndexTrimmed: output (outX_, outY_) back to
  // physical framebuffer coordinates.
  uint32_t bx, by;
  switch (p_.rot & 3u) {
    default:
    case 0:
      bx = static_cast<uint32_t>(p_.srcX) + outX_;
      by = static_cast<uint32_t>(p_.srcY) + outY_;
      break;
    case 1:
      bx = static_cast<uint32_t>(p_.srcX) + outY_;
      by = static_cast<uint32_t>(p_.srcY) + p_.srcH - 1u - outX_;
      break;
    case 2:
      bx = static_cast<uint32_t>(p_.srcX) + p_.srcW - 1u - outX_;
      by = static_cast<uint32_t>(p_.srcY) + p_.srcH - 1u - outY_;
      break;
    case 3:
      bx = static_cast<uint32_t>(p_.srcX) + p_.srcW - 1u - outY_;
      by = static_cast<uint32_t>(p_.srcY) + outX_;
      break;
  }
  uint16_t want =
      expectedRgb565(p_.pattern, by * p_.pattern.physW + bx, p_.rbSwap);
  if (((px ^ want) & mask_) != 0u) {
    if (mismatchCount_ == 0u) {
      firstBadX_ = static_cast<uint16_t>(bx);
      firstBadY_ = static_cast<uint16_t>(by);
      firstGot_ = px & mask_;
      firstWant_ = want & mask_;
    }
    mismatchCount_++;
  }
  pixelCount_++;
  if (++outX_ >= outW_) {
    outX_ = 0;
    outY_++;
  }
}

bool FrameVerifier::finish() {
  return !streamError_ && mismatchCount_ == 0u &&
         pixelCount_ == static_cast<uint32_t>(outW_) * outH_;
}

uint32_t textMismatchCount(uint32_t seed, uint16_t cols, uint16_t rows,
                           const uint8_t* data) {
  uint32_t bad = 0;
  char line[64];
  for (uint16_t row = 0; row < rows; row++) {
    buildTextRow(seed, row, cols, line);
    for (uint16_t col = 0; col < cols; col++) {
      if (static_cast<uint8_t>(line[col]) != data[row * cols + col]) bad++;
    }
  }
  return bad;
}

}  // namespace testrig
