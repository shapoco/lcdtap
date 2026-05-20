#include <lcdtap/lcdtap.hpp>

#include <cstring>
#include <new>

#include "controller_base.hpp"
#include "ssd1306_controller.hpp"
#include "ssd1331_controller.hpp"
#include "st7789_controller.hpp"

namespace lcdtap {

//=============================================================================
// getDefaultConfig
//=============================================================================
DumpConfig getDefaultDumpConfig() { return {}; }

void getDefaultConfig(ControllerType type, LcdTapConfig* cfg) {
  *cfg = {};
  switch (type) {
    case ControllerType::ST7789:
      cfg->controller = ControllerType::ST7789;
      cfg->lcdWidth = 240;
      cfg->lcdHeight = 320;
      cfg->interfaceFormat = InterfaceFormat::RGB565_BE;
      cfg->dviWidth = 640;
      cfg->dviHeight = 480;
      cfg->scaleMode = ScaleMode::FIT;
      cfg->inverted = true;
      cfg->swapRB = false;
      cfg->outputRotation = 3;
      cfg->forcePowerOn = false;
      break;
    case ControllerType::SSD1306:
      cfg->controller = ControllerType::SSD1306;
      cfg->lcdWidth = 128;
      cfg->lcdHeight = 64;
      cfg->interfaceFormat = InterfaceFormat::GRAY1_VPACK8_H2L;
      cfg->dviWidth = 640;
      cfg->dviHeight = 480;
      cfg->scaleMode = ScaleMode::FIT;
      cfg->inverted = false;
      cfg->swapRB = false;
      cfg->outputRotation = 0;
      cfg->forcePowerOn = false;
      break;
    case ControllerType::SSD1331:
      cfg->controller = ControllerType::SSD1331;
      cfg->lcdWidth = 96;
      cfg->lcdHeight = 64;
      cfg->interfaceFormat = InterfaceFormat::RGB332;
      cfg->dviWidth = 640;
      cfg->dviHeight = 480;
      cfg->scaleMode = ScaleMode::FIT;
      cfg->inverted = false;
      cfg->swapRB = false;
      cfg->outputRotation = 0;
      cfg->forcePowerOn = false;
      break;
  }
}

//=============================================================================
// ControllerBase common implementation
//=============================================================================

void ControllerBase::calcScaleParams() {
  uint16_t dviW = config.dviWidth;
  uint16_t dviH = config.dviHeight;
  // rot=1,3: LCD width and height appear swapped
  uint16_t lcdW = ((outputRotation & 1) ? config.lcdHeight : config.lcdWidth);
  uint16_t lcdH = ((outputRotation & 1) ? config.lcdWidth : config.lcdHeight);

  switch (config.scaleMode) {
    case ScaleMode::STRETCH:
      displayX = 0;
      displayY = 0;
      displayW = dviW;
      displayH = dviH;
      break;

    case ScaleMode::FIT:
      // lcdW/lcdH > dviW/dviH (landscape LCD) ↔ lcdW*dviH > dviW*lcdH
      if ((uint32_t)lcdW * dviH > (uint32_t)dviW * lcdH) {
        displayW = dviW;
        displayH = (uint16_t)((uint32_t)dviW * lcdH / lcdW);
      } else {
        displayH = dviH;
        displayW = (uint16_t)((uint32_t)dviH * lcdW / lcdH);
      }
      displayX = (dviW - displayW) / 2;
      displayY = (dviH - displayH) / 2;
      break;

    case ScaleMode::PIXEL_PERFECT: {
      uint16_t scaleH = dviW / lcdW;
      uint16_t scaleV = dviH / lcdH;
      uint16_t scale = scaleH < scaleV ? scaleH : scaleV;
      if (scale == 0) scale = 1;
      displayW = lcdW * scale;
      displayH = lcdH * scale;
      displayX = (dviW - displayW) / 2;
      displayY = (dviH - displayH) / 2;
      break;
    }
  }

  hStep = ((uint32_t)lcdW << 16) / displayW;
  vStep = ((uint32_t)lcdH << 16) / displayH;
}

void ControllerBase::log(const char* msg) const {
  if (host.log) host.log(host.userData, msg);
}

void ControllerBase::resetCommon() {
  sleeping = true;
  displayOn = false;
  inverted = false;
  cachedLittleEndian = false;
  interfaceFormat = config.interfaceFormat;
  currentCmd = 0x00;
  cmdDataLen = 0;
  casetXS = 0;
  casetXE = config.lcdWidth - 1;
  rasetYS = 0;
  rasetYE = config.lcdHeight - 1;
  ramwrX = 0;
  ramwrY = 0;
  ramwrBufLen = 0;
  updateWriteCache();
}

// Process all RAMWR data at once (moves switch(interfaceFormat) outside the
// loop)
void ControllerBase::processRamwrData(const uint8_t* data, uint32_t numBytes,
                                      uint32_t stride) {
  int32_t i = 0;
  int32_t length = numBytes * stride;

  switch (interfaceFormat) {
    case InterfaceFormat::RGB332: {
      // 1 byte = 1 pixel: {R[2:0], G[2:0], B[1:0]}
      // Expand 3→5 bits: (r<<2)|(r>>1)  3→6 bits: (g<<3)|g  2→5 bits:
      // (b<<3)|(b<<1)|(b>>1)
      while (i < length) {
        uint8_t b = data[i];
        i += stride;
        uint_fast16_t r5 = (b >> 5) & 0x07u;
        uint_fast16_t g3 = (b >> 2) & 0x07u;
        uint_fast16_t b2 = b & 0x03u;
        r5 = (r5 << 2) | (r5 >> 1);
        uint_fast16_t g6 = (g3 << 3) | g3;
        uint_fast16_t b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
        writePixelRgb565(static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5));
      }
      break;
    }

    case InterfaceFormat::RGB111_HPACK2_H2L_RA8: {
      // 1 byte = 2 pixels: bits[5:3] = younger pixel, bits[2:0] = older pixel
      // 3-bit RGB -> RGB565 lookup (R:1->5bit, G:1->6bit, B:1->5bit)
      static const uint16_t kLut[8] = {
          0x0000, 0x001F, 0x07E0, 0x07FF, 0xF800, 0xF81F, 0xFFE0, 0xFFFF,
      };
      while (i < length) {
        uint8_t b = data[i];
        i += stride;
        writePixelRgb565(kLut[(b >> 3) & 0x07u]);
        writePixelRgb565(kLut[b & 0x07u]);
      }
      break;
    }

    case InterfaceFormat::RGB444_HPACK2_H2L_BE:
      // byte0: R1[3:0] G1[3:0]  byte1: B1[3:0] R2[3:0]  byte2: G2[3:0] B2[3:0]
      // 4bit→5bit: x<<1 (MSB-align; LSB zeroed)  4bit→6bit: x<<2 (MSB-align;
      // lower 2 bits zeroed) Drain leftover bytes (0–2 bytes)
      while (ramwrBufLen > 0 && i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
        if (ramwrBufLen == 3) {
          uint_fast16_t b0 = ramwrBuf[0], b1 = ramwrBuf[1], b2 = ramwrBuf[2];
          uint_fast16_t pixel0 = 0;
          pixel0 |= (b0 << 8) & 0xF000;  // R1
          pixel0 |= (b0 << 7) & 0x0780;  // G1
          pixel0 |= (b1 >> 3) & 0x001E;  // B1
          writePixelRgb565(pixel0);
          uint_fast16_t pixel1 = 0;
          pixel1 |= (b1 << 12) & 0xF000;  // R2
          pixel1 |= (b2 << 3) & 0x0780;   // G2
          pixel1 |= (b2 << 1) & 0x001E;   // B2
          writePixelRgb565(pixel1);
          ramwrBufLen = 0;
        }
      }
      // Tight loop: 3 bytes → 2 pixels
      while (i + stride * 3 <= length) {
        uint8_t b0 = data[i];
        i += stride;
        uint8_t b1 = data[i];
        i += stride;
        uint8_t b2 = data[i];
        i += stride;
        uint_fast16_t pixel0 = 0;
        pixel0 |= (b0 << 8) & 0xF000;  // R1
        pixel0 |= (b0 << 7) & 0x0780;  // G1
        pixel0 |= (b1 >> 3) & 0x001E;  // B1
        writePixelRgb565(pixel0);
        uint_fast16_t pixel1 = 0;
        pixel1 |= (b1 << 12) & 0xF000;  // R2
        pixel1 |= (b2 << 3) & 0x0780;   // G2
        pixel1 |= (b2 << 1) & 0x001E;   // B2
        writePixelRgb565(pixel1);
      }
      // Save remainder
      while (i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
      }
      break;

    case InterfaceFormat::RGB565_BE:
      // Drain leftover 1 byte
      if (ramwrBufLen == 1 && i < length) {
        uint_fast16_t b0 = ramwrBuf[0];
        uint_fast16_t b1 = data[i];
        uint_fast16_t pixel =
            cachedLittleEndian ? (b0 | (b1 << 8)) : ((b0 << 8) | b1);
        writePixelRgb565(pixel);
        i += stride;
        ramwrBufLen = 0;
      }
      // Tight loop: 2 bytes → 1 pixel
      while (i + stride * 2 <= length) {
        uint_fast16_t b0 = data[i];
        i += stride;
        uint_fast16_t b1 = data[i];
        i += stride;
        uint_fast16_t pixel =
            cachedLittleEndian ? (b0 | (b1 << 8)) : ((b0 << 8) | b1);
        writePixelRgb565(pixel);
      }
      // Save remainder
      if (i < length) {
        ramwrBuf[0] = data[i];
        ramwrBufLen = 1;
      }
      break;

    case InterfaceFormat::RGB666_UNPACK_LA8_BE:
      // Upper 6 bits of each byte are significant (lower 2 bits = 0)
      // R5 = byte0>>3, G6 = byte1>>2, B5 = byte2>>3 maps directly to RGB565
      // Drain leftover bytes (0–2 bytes)
      while (ramwrBufLen > 0 && i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
        if (ramwrBufLen == 3) {
          uint_fast16_t pixel = 0;
          pixel |= (ramwrBuf[0] & 0xFCu) << 8;  // R5
          pixel |= (ramwrBuf[1] & 0xFCu) << 3;  // G6
          pixel |= (ramwrBuf[2] & 0xFCu) >> 3;  // B5
          writePixelRgb565(static_cast<uint16_t>(pixel));
          ramwrBufLen = 0;
        }
      }
      // Tight loop: 3 bytes → 1 pixel
      while (i + stride * 3 <= length) {
        uint_fast16_t pixel = 0;
        pixel |= (data[i] & 0xFCu) << 8;  // R5
        i += stride;
        pixel |= (data[i] & 0xFCu) << 3;  // G6
        i += stride;
        pixel |= (data[i] & 0xFCu) >> 3;  // B5
        i += stride;
        writePixelRgb565(static_cast<uint16_t>(pixel));
      }
      // Save remainder
      while (i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
      }
      break;

    case InterfaceFormat::RGB666_UNPACK_RA8_BE:
      // 3 bytes = 1 pixel: color data in bits[5:0] of each byte (right-aligned)
      // R5 = byte0[5:1], G6 = byte1[5:0], B5 = byte2[5:1]
      // (bit[0] of R and B channels is hardware-unused on SSD1331)
      // Drain leftover bytes (0-2 bytes)
      while (ramwrBufLen > 0 && i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
        if (ramwrBufLen == 3) {
          uint_fast16_t pixel = 0;
          pixel |= static_cast<uint_fast16_t>((ramwrBuf[0] >> 1) & 0x1Fu)
                   << 11;                                                 // R5
          pixel |= static_cast<uint_fast16_t>(ramwrBuf[1] & 0x3Fu) << 5;  // G6
          pixel |=
              static_cast<uint_fast16_t>((ramwrBuf[2] >> 1) & 0x1Fu);  // B5
          writePixelRgb565(static_cast<uint16_t>(pixel));
          ramwrBufLen = 0;
        }
      }
      // Tight loop: 3 bytes → 1 pixel
      while (i + stride * 3 <= length) {
        uint_fast16_t pixel = 0;
        pixel |= static_cast<uint_fast16_t>((data[i] >> 1) & 0x1Fu)
                 << 11;  // R5
        i += stride;
        pixel |= static_cast<uint_fast16_t>(data[i] & 0x3Fu) << 5;  // G6
        i += stride;
        pixel |= static_cast<uint_fast16_t>((data[i] >> 1) & 0x1Fu);  // B5
        i += stride;
        writePixelRgb565(static_cast<uint16_t>(pixel));
      }
      // Save remainder
      while (i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
      }
      break;
  }
}

// Process a data byte stream: RAMWR data is handled in bulk, others byte by
// byte
void ControllerBase::feedData(const uint8_t* data, uint32_t numBytes,
                              uint32_t stride) {
  if (stride == 0) stride = 1;  // safety guard
  if (isRamWriteCommand()) {
    processRamwrData(data, numBytes, stride);
  } else {
    for (uint32_t i = 0; i < numBytes * stride; i += stride) {
      feedDataByte(data[i]);
    }
  }
}

//=============================================================================
// LcdTap
//=============================================================================

LcdTap::LcdTap(const LcdTapConfig& config, const HostInterface& host)
    : impl_(nullptr) {
  if (!host.alloc || !host.free) return;

  ControllerBase* ctrl = nullptr;
  switch (config.controller) {
    case ControllerType::ST7789:
      ctrl = new (std::nothrow) St7789Controller();
      break;
    case ControllerType::SSD1306:
      ctrl = new (std::nothrow) Ssd1306Controller();
      break;
    case ControllerType::SSD1331:
      ctrl = new (std::nothrow) Ssd1331Controller();
      break;
  }
  if (!ctrl) return;

  ctrl->config = config;
  ctrl->host = host;
  ctrl->status = Status::OK;
  ctrl->hwReset = false;
  ctrl->framebuf = nullptr;
  ctrl->outputRotation = config.outputRotation & 3u;

  if (config.lcdWidth == 0 || config.lcdHeight == 0 || config.dviWidth == 0 ||
      config.dviHeight == 0) {
    ctrl->status = Status::INVALID_PARAM;
    impl_ = ctrl;
    return;
  }

  size_t fbSize = (size_t)config.lcdWidth * config.lcdHeight * sizeof(uint16_t);
  ctrl->framebuf = static_cast<uint16_t*>(host.alloc(fbSize));
  if (!ctrl->framebuf) {
    ctrl->status = Status::OUT_OF_MEMORY;
    impl_ = ctrl;
    return;
  }

  impl_ = ctrl;
  ctrl->calcScaleParams();
  ctrl->softReset();
}

LcdTap::~LcdTap() {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->framebuf) ctrl->host.free(ctrl->framebuf);
  delete ctrl;
}

Status LcdTap::getStatus() const {
  if (!impl_) return Status::OUT_OF_MEMORY;
  return static_cast<const ControllerBase*>(impl_)->status;
}

void LcdTap::inputReset(bool assert) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK) return;
  ctrl->hwReset = assert;
  if (!assert) {
    if (dumpState_ == DumpState::ACTIVE) dumpPush(DUMP_EVENT_HW_RESET);
    ctrl->softReset();
    ctrl->log("HW RESET released");
  }
}

void LcdTap::inputCommand(uint8_t byte) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK || ctrl->hwReset) return;
  ctrl->dispatchCommand(byte);
  if (dumpState_ == DumpState::ACTIVE) dumpPush(byte);
}

void LcdTap::inputData(const uint8_t* data, uint32_t numBytes,
                       uint32_t stride) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK || ctrl->hwReset) return;
  ctrl->feedData(data, numBytes, stride);
  if (dumpState_ == DumpState::ACTIVE) {
    const uint32_t s = (stride == 0) ? 1u : stride;
    for (uint32_t i = 0; i < numBytes && dumpState_ == DumpState::ACTIVE; ++i)
      dumpPush(0x100u | data[i * s]);
  }
}

//=============================================================================
// LcdTap dump API
//=============================================================================

void LcdTap::dumpStart(const DumpConfig& dumpCfg) {
  dumpConfig_ = dumpCfg;
  dumpBuffSize_ = 0;
  dumpState_ = DumpState::WAIT;
}

DumpState LcdTap::dumpGetState() const { return dumpState_; }

uint16_t LcdTap::dumpGetSize() const { return dumpBuffSize_; }

void LcdTap::dumpForceTrigger() {
  if (dumpState_ == DumpState::WAIT) dumpState_ = DumpState::ACTIVE;
}

void LcdTap::dumpAbort() { dumpState_ = DumpState::COMPLETE; }

const uint16_t* LcdTap::dumpGetBuffer() const { return dumpBuffer_; }

void LcdTap::fillScanline(uint16_t dviLine, uint16_t* dst) const {
  if (!impl_) return;
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  if (ctrl->status != Status::OK || !ctrl->framebuf) return;

  const uint16_t dviW = ctrl->config.dviWidth;

  // Display off, sleeping, or in the vertical black border region
  bool powerOff =
      !ctrl->config.forcePowerOn && (ctrl->sleeping || !ctrl->displayOn);
  if (powerOff || dviLine < ctrl->displayY ||
      dviLine >= ctrl->displayY + ctrl->displayH) {
    memset(dst, 0, dviW * sizeof(uint16_t));
    return;
  }

  // Vertical mapping: compute LCD row via fixed-point multiply
  const uint32_t lcdRowOut =
      ((uint32_t)(dviLine - ctrl->displayY) * ctrl->vStep) >> 16;

  uint16_t* d = dst;

  // Left black border
  memset(d, 0, (size_t)ctrl->displayX * sizeof(uint16_t));
  d += ctrl->displayX;

  // Active area: horizontal scaling + brightness inversion + rotation
  // Inversion: XOR all RGB565 bits → (31-R, 63-G, 31-B) inverts each channel
  const uint16_t inv = ctrl->inverted ? 0xFFFFu : 0u;
  const uint16_t* fb = ctrl->framebuf;
  const uint32_t physW = ctrl->config.lcdWidth;
  const uint32_t physH = ctrl->config.lcdHeight;
  uint32_t hAccum = 0;

  switch (ctrl->outputRotation) {
    default:
    case 0: {
      // rot=0: normal (row-major readout)
      const uint16_t* srcRow = fb + (uint32_t)lcdRowOut * physW;
      for (uint16_t x = 0; x < ctrl->displayW; ++x) {
        *d++ = srcRow[hAccum >> 16] ^ inv;
        hAccum += ctrl->hStep;
      }
      break;
    }
    case 1: {
      // rot=1: 90° CW  — (lcdRowOut, lcdColOut) → ((physH-1-lcdColOut),
      // lcdRowOut)
      for (uint16_t x = 0; x < ctrl->displayW; ++x) {
        uint16_t lcdColOut = static_cast<uint16_t>(hAccum >> 16);
        *d++ = fb[(uint32_t)(physH - 1u - lcdColOut) * physW + lcdRowOut] ^ inv;
        hAccum += ctrl->hStep;
      }
      break;
    }
    case 2: {
      // rot=2: 180° — (lcdRowOut, lcdColOut) → ((physH-1-lcdRowOut),
      // (physW-1-lcdColOut))
      const uint16_t* srcRow = fb + (uint32_t)(physH - 1u - lcdRowOut) * physW;
      for (uint16_t x = 0; x < ctrl->displayW; ++x) {
        uint16_t lcdColOut = static_cast<uint16_t>(hAccum >> 16);
        *d++ = srcRow[physW - 1u - lcdColOut] ^ inv;
        hAccum += ctrl->hStep;
      }
      break;
    }
    case 3: {
      // rot=3: 270° CW — (lcdRowOut, lcdColOut) → (lcdColOut,
      // (physW-1-lcdRowOut))
      for (uint16_t x = 0; x < ctrl->displayW; ++x) {
        uint16_t lcdColOut = static_cast<uint16_t>(hAccum >> 16);
        *d++ = fb[(uint32_t)lcdColOut * physW + (physW - 1u - lcdRowOut)] ^ inv;
        hAccum += ctrl->hStep;
      }
      break;
    }
  }

  // Right black border
  memset(d, 0,
         (size_t)(dviW - ctrl->displayX - ctrl->displayW) * sizeof(uint16_t));
}

LcdTapConfig LcdTap::getConfig() const {
  if (!impl_) return {};
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  LcdTapConfig cfg = ctrl->config;
  cfg.outputRotation = ctrl->outputRotation;
  return cfg;
}

Status LcdTap::updateConfig(const LcdTapConfig& cfg) {
  if (!impl_) return Status::NOT_READY;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK) return ctrl->status;

  if (cfg.lcdWidth == 0 || cfg.lcdHeight == 0 || cfg.dviWidth == 0 ||
      cfg.dviHeight == 0) {
    return Status::INVALID_PARAM;
  }

  // If the controller type changes, destroy the old controller and create a new
  // one.  The new controller starts from a clean state (sleeping/displayOn are
  // not preserved because the new controller has not yet received any
  // commands).
  if (cfg.controller != ctrl->config.controller) {
    HostInterface host = ctrl->host;
    if (ctrl->framebuf) host.free(ctrl->framebuf);
    delete ctrl;
    impl_ = nullptr;

    switch (cfg.controller) {
      case ControllerType::ST7789:
        ctrl = new (std::nothrow) St7789Controller();
        break;
      case ControllerType::SSD1306:
        ctrl = new (std::nothrow) Ssd1306Controller();
        break;
      case ControllerType::SSD1331:
        ctrl = new (std::nothrow) Ssd1331Controller();
        break;
      default: return Status::OUT_OF_MEMORY;
    }
    if (!ctrl) return Status::OUT_OF_MEMORY;

    ctrl->host = host;
    ctrl->status = Status::OK;
    ctrl->hwReset = false;
    ctrl->framebuf = nullptr;
    impl_ = ctrl;

    size_t fbSize = (size_t)cfg.lcdWidth * cfg.lcdHeight * sizeof(uint16_t);
    ctrl->framebuf = static_cast<uint16_t*>(host.alloc(fbSize));
    if (!ctrl->framebuf) return Status::OUT_OF_MEMORY;

    ctrl->config = cfg;
    ctrl->outputRotation = cfg.outputRotation & 3u;
    ctrl->interfaceFormat = cfg.interfaceFormat;
    ctrl->calcScaleParams();
    ctrl->softReset();
    return Status::OK;
  }

  // Same controller type: update config in-place.
  // Allocate new framebuffer before freeing the old one.
  size_t currFbSize =
      (size_t)ctrl->config.lcdWidth * ctrl->config.lcdHeight * sizeof(uint16_t);
  size_t newFbSize = (size_t)cfg.lcdWidth * cfg.lcdHeight * sizeof(uint16_t);
  if (newFbSize > currFbSize) {
    uint16_t* newFb = static_cast<uint16_t*>(ctrl->host.alloc(newFbSize));
    if (!newFb) return Status::OUT_OF_MEMORY;
    ctrl->host.free(ctrl->framebuf);
    ctrl->framebuf = newFb;
  }

  bool sleeping = ctrl->sleeping;
  bool displayOn = ctrl->displayOn;
  ctrl->config = cfg;
  ctrl->outputRotation = cfg.outputRotation & 3u;
  ctrl->interfaceFormat = cfg.interfaceFormat;
  ctrl->calcScaleParams();
  ctrl->softReset();
  ctrl->sleeping = sleeping;
  ctrl->displayOn = displayOn;
  return Status::OK;
}

uint16_t* LcdTap::getFramebuf() {
  if (!impl_) return nullptr;
  return static_cast<ControllerBase*>(impl_)->framebuf;
}

void LcdTap::setDisplayOn(bool on) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK) return;
  ctrl->sleeping = !on;
  ctrl->displayOn = on;
}

void LcdTap::setOutputRotation(int rot) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK) return;
  ctrl->outputRotation = static_cast<uint8_t>(rot & 3);
  ctrl->calcScaleParams();
}

}  // namespace lcdtap
