#include <lcdtap/lcdtap.hpp>

#include <cstring>
#include <new>

#include "controller_base.hpp"
#include "ssd1309_controller.hpp"
#include "st7789_controller.hpp"

namespace lcdtap {

//=============================================================================
// getDefaultConfig
//=============================================================================
void getDefaultConfig(ControllerType type, LcdTapConfig* cfg) {
  *cfg = {};
  switch (type) {
    case ControllerType::ST7789:
      cfg->controller = ControllerType::ST7789;
      cfg->lcdWidth = 240;
      cfg->lcdHeight = 320;
      cfg->pixelFormat = PixelFormat::RGB565;
      cfg->dviWidth = 640;
      cfg->dviHeight = 480;
      cfg->scaleMode = ScaleMode::FIT;
      cfg->invertInvPolarity = false;
      break;
    case ControllerType::SSD1309:
      cfg->controller = ControllerType::SSD1309;
      cfg->lcdWidth = 128;
      cfg->lcdHeight = 64;
      cfg->pixelFormat = PixelFormat::MONO_VPACK;
      cfg->dviWidth = 640;
      cfg->dviHeight = 480;
      cfg->scaleMode = ScaleMode::FIT;
      cfg->invertInvPolarity = false;
      break;
  }
}

//=============================================================================
// ControllerBase 共通実装
//=============================================================================

void ControllerBase::calcScaleParams() {
  uint16_t dviW = config.dviWidth;
  uint16_t dviH = config.dviHeight;
  uint16_t lcdW = config.lcdWidth;
  uint16_t lcdH = config.lcdHeight;

  switch (config.scaleMode) {
    case ScaleMode::STRETCH:
      displayX = 0;
      displayY = 0;
      displayW = dviW;
      displayH = dviH;
      break;

    case ScaleMode::FIT:
      // lcdW/lcdH > dviW/dviH (横長LCD) ↔ lcdW*dviH > dviW*lcdH
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
  pixelFormat = PixelFormat::RGB565;
  currentCmd = 0x00;
  cmdDataLen = 0;
  casetXS = 0;
  casetXE = config.lcdWidth - 1;
  rasetYS = 0;
  rasetYE = config.lcdHeight - 1;
  ramwrX = 0;
  ramwrY = 0;
  ramwrBufLen = 0;
  memset(framebuf, 0,
         (size_t)config.lcdWidth * config.lcdHeight * sizeof(uint16_t));
  updateWriteCache();
}

// RGB565 値を 1 ピクセルとしてフレームバッファに書く (MADCTL BGR 考慮)
[[gnu::always_inline]] void ControllerBase::writePixelRgb565(uint16_t px) {
  if (cachedBGR) {  // BGR: R[15:11] と B[4:0] を入れ替える
    uint16_t r = (px >> 11) & 0x1Fu;
    uint16_t g = (px >> 5) & 0x3Fu;
    uint16_t b = px & 0x1Fu;
    px = static_cast<uint16_t>((b << 11) | (g << 5) | r);
  }
  *writePtr = px;
  writePtr += cachedHStep;
  if (++ramwrX > casetXE) {
    ramwrX = casetXS;
    if (++ramwrY > rasetYE) {
      ramwrY = rasetYS;
    }
    writePtr = framebuf + physIndex(ramwrX, ramwrY);
  }
}

// RAMWR データをまとめて処理する (switch(pixelFormat) をループ外に出す)
void ControllerBase::processRamwrData(const uint8_t* data, size_t length) {
  size_t i = 0;

  switch (pixelFormat) {
    case PixelFormat::RGB565:
      // 残余 1 バイトの drain
      if (ramwrBufLen == 1 && i < length) {
        writePixelRgb565(static_cast<uint16_t>((ramwrBuf[0] << 8) | data[i++]));
        ramwrBufLen = 0;
      }
      // タイトループ: 2 バイト → 1 ピクセル (ビッグエンディアン)
      while (i + 2 <= length) {
        writePixelRgb565(static_cast<uint16_t>((data[i] << 8) | data[i + 1]));
        i += 2;
      }
      // 端数保存
      if (i < length) {
        ramwrBuf[0] = data[i];
        ramwrBufLen = 1;
      }
      break;

    case PixelFormat::RGB444:
      // byte0: R1[3:0] G1[3:0]  byte1: B1[3:0] R2[3:0]  byte2: G2[3:0] B2[3:0]
      // 4bit→5bit: (x<<1)|(x>>3)  4bit→6bit: (x<<2)|(x>>2)
      // 残余 (0〜2 バイト) の drain
      while (ramwrBufLen > 0 && i < length) {
        ramwrBuf[ramwrBufLen++] = data[i++];
        if (ramwrBufLen == 3) {
          uint8_t b0 = ramwrBuf[0], b1 = ramwrBuf[1], b2 = ramwrBuf[2];
          uint8_t r1 = b0 >> 4, g1 = b0 & 0xFu, b1v = b1 >> 4;
          uint8_t r2 = b1 & 0xFu, g2 = b2 >> 4, b2v = b2 & 0xFu;
          writePixelRgb565(static_cast<uint16_t>(
              ((uint16_t)(r1 << 1 | r1 >> 3) << 11) |
              ((uint16_t)(g1 << 2 | g1 >> 2) << 5) | (b1v << 1 | b1v >> 3)));
          writePixelRgb565(static_cast<uint16_t>(
              ((uint16_t)(r2 << 1 | r2 >> 3) << 11) |
              ((uint16_t)(g2 << 2 | g2 >> 2) << 5) | (b2v << 1 | b2v >> 3)));
          ramwrBufLen = 0;
        }
      }
      // タイトループ: 3 バイト → 2 ピクセル
      while (i + 3 <= length) {
        uint8_t b0 = data[i], b1 = data[i + 1], b2 = data[i + 2];
        i += 3;
        uint8_t r1 = b0 >> 4, g1 = b0 & 0xFu, b1v = b1 >> 4;
        uint8_t r2 = b1 & 0xFu, g2 = b2 >> 4, b2v = b2 & 0xFu;
        writePixelRgb565(static_cast<uint16_t>(
            ((uint16_t)(r1 << 1 | r1 >> 3) << 11) |
            ((uint16_t)(g1 << 2 | g1 >> 2) << 5) | (b1v << 1 | b1v >> 3)));
        writePixelRgb565(static_cast<uint16_t>(
            ((uint16_t)(r2 << 1 | r2 >> 3) << 11) |
            ((uint16_t)(g2 << 2 | g2 >> 2) << 5) | (b2v << 1 | b2v >> 3)));
      }
      // 端数保存
      while (i < length) ramwrBuf[ramwrBufLen++] = data[i++];
      break;

    case PixelFormat::RGB666:
      // 各バイト上位 6bit が有効 (下位 2bit = 0)
      // R5 = byte0>>3, G6 = byte1>>2, B5 = byte2>>3 で直接 RGB565 に変換できる
      // 残余 (0〜2 バイト) の drain
      while (ramwrBufLen > 0 && i < length) {
        ramwrBuf[ramwrBufLen++] = data[i++];
        if (ramwrBufLen == 3) {
          writePixelRgb565(static_cast<uint16_t>(
              ((uint16_t)(ramwrBuf[0] & 0xF8u) << 8) |
              ((uint16_t)(ramwrBuf[1] & 0xFCu) << 3) | (ramwrBuf[2] >> 3)));
          ramwrBufLen = 0;
        }
      }
      // タイトループ: 3 バイト → 1 ピクセル
      while (i + 3 <= length) {
        writePixelRgb565(static_cast<uint16_t>(
            ((uint16_t)(data[i] & 0xF8u) << 8) |
            ((uint16_t)(data[i + 1] & 0xFCu) << 3) | (data[i + 2] >> 3)));
        i += 3;
      }
      // 端数保存
      while (i < length) ramwrBuf[ramwrBufLen++] = data[i++];
      break;
  }
}

// データバイト列の処理: RAMWR は一括、それ以外は 1 バイトずつ
[[gnu::always_inline]] void ControllerBase::feedData(const uint8_t* data,
                                                     size_t length) {
  if (isRamWriteCommand()) {
    processRamwrData(data, length);
  } else {
    for (size_t i = 0; i < length; ++i) feedDataByte(data[i]);
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
    case ControllerType::SSD1309:
      ctrl = new (std::nothrow) Ssd1309Controller();
      break;
  }
  if (!ctrl) return;

  ctrl->config = config;
  ctrl->host = host;
  ctrl->status = Status::OK;
  ctrl->hwReset = false;
  ctrl->framebuf = nullptr;

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
    ctrl->softReset();
    ctrl->log("HW RESET released");
  }
}

void LcdTap::inputCommand(uint8_t byte) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK || ctrl->hwReset) return;
  ctrl->dispatchCommand(byte);
}

void LcdTap::inputData(const uint8_t* data, size_t length) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK || ctrl->hwReset) return;
  ctrl->feedData(data, length);
}

void LcdTap::fillScanline(uint16_t dviLine, uint16_t* dst) const {
  if (!impl_) return;
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  if (ctrl->status != Status::OK || !ctrl->framebuf) return;

  const uint16_t dviW = ctrl->config.dviWidth;
  const uint16_t lcdW = ctrl->config.lcdWidth;

  // 表示オフ・スリープ中、または垂直方向の黒帯
  if (ctrl->sleeping || !ctrl->displayOn || dviLine < ctrl->displayY ||
      dviLine >= ctrl->displayY + ctrl->displayH) {
    memset(dst, 0, dviW * sizeof(uint16_t));
    return;
  }

  // 垂直マッピング: 固定小数点乗算で LCD 行を求める
  uint16_t lcdRow = static_cast<uint16_t>(
      ((uint32_t)(dviLine - ctrl->displayY) * ctrl->vStep) >> 16);
  const uint16_t* srcRow = ctrl->framebuf + (uint32_t)lcdRow * lcdW;

  uint16_t* d = dst;

  // 左の黒帯
  memset(d, 0, (size_t)ctrl->displayX * sizeof(uint16_t));
  d += ctrl->displayX;

  // アクティブ領域: 水平スケーリング + 輝度反転
  // 反転: RGB565 全ビット XOR → (31-R, 63-G, 31-B) で各チャネル反転
  uint32_t hAccum = 0;
  if (ctrl->inverted) {
    for (uint16_t x = 0; x < ctrl->displayW; ++x) {
      *d++ = srcRow[hAccum >> 16] ^ 0xFFFFu;
      hAccum += ctrl->hStep;
    }
  } else {
    for (uint16_t x = 0; x < ctrl->displayW; ++x) {
      *d++ = srcRow[hAccum >> 16];
      hAccum += ctrl->hStep;
    }
  }

  // 右の黒帯
  memset(d, 0,
         (size_t)(dviW - ctrl->displayX - ctrl->displayW) * sizeof(uint16_t));
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

}  // namespace lcdtap
