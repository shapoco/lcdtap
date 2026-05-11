#include <spilcd2dvi/spilcd2dvi.hpp>

#include <cstring>

namespace sl2d {

//=============================================================================
// コマンドコード
//=============================================================================
static constexpr uint8_t CMD_NOP = 0x00;
static constexpr uint8_t CMD_SWRESET = 0x01;
static constexpr uint8_t CMD_SLPOUT = 0x11;
static constexpr uint8_t CMD_INVOFF = 0x20;
static constexpr uint8_t CMD_INVON = 0x21;
static constexpr uint8_t CMD_DISPON = 0x29;
static constexpr uint8_t CMD_CASET = 0x2A;
static constexpr uint8_t CMD_RASET = 0x2B;
static constexpr uint8_t CMD_RAMWR = 0x2C;
static constexpr uint8_t CMD_MADCTL = 0x36;
static constexpr uint8_t CMD_COLMOD = 0x3A;

//=============================================================================
// 内部実装構造体 (PIMPL)
//=============================================================================
struct Impl {
  Sl2dConfig config;
  HostInterface host;
  Status status;
  bool hwReset;

  uint16_t* framebuf;  // lcdWidth × lcdHeight × sizeof(uint16_t) の RGB565
                       // フレームバッファ

  // ST7789 レジスタ
  bool sleeping;
  bool displayOn;
  bool inverted;
  PixelFormat pixelFormat;
  uint8_t madctl;

  // コマンドステートマシン
  uint8_t currentCmd;
  uint8_t cmdDataLen;  // 現コマンドで受け取ったデータバイト数

  // RAMWR 状態
  uint16_t casetXS;     // CASET 開始列 (論理座標)
  uint16_t casetXE;     // CASET 終了列 (論理座標、inclusive)
  uint16_t rasetYS;     // RASET 開始行 (論理座標)
  uint16_t rasetYE;     // RASET 終了行 (論理座標、inclusive)
  uint16_t ramwrX;      // 現在の書き込み位置 X (論理座標)
  uint16_t ramwrY;      // 現在の書き込み位置 Y (論理座標)
  uint8_t ramwrBuf[3];  // 半端バイト蓄積 (最大 3 バイト)
  uint8_t ramwrBufLen;

  // スケーリングパラメータ (コンストラクタで計算済み)
  uint16_t displayX;  // DVI上のLCD表示領域 開始X座標
  uint16_t displayY;  // DVI上のLCD表示領域 開始Y座標
  uint16_t displayW;  // DVI上のLCD表示領域の幅
  uint16_t displayH;  // DVI上のLCD表示領域の高さ
  uint32_t hStep;  // 水平固定小数点ステップ (16.16形式: lcdW<<16 / displayW)
  uint32_t vStep;  // 垂直固定小数点ステップ (16.16形式: lcdH<<16 / displayH)

  // MADCTL 確定時にキャッシュする値 (updateWriteCache() で更新)
  bool cachedBGR;
  int32_t cachedHStep;
  uint16_t* writePtr;

  // --- ヘルパー ---

  // MADCTL MV ビットを考慮した論理幅/高さ
  uint16_t logicalWidth() const {
    return ((madctl >> 5) & 1) ? config.lcdHeight : config.lcdWidth;
  }
  uint16_t logicalHeight() const {
    return ((madctl >> 5) & 1) ? config.lcdWidth : config.lcdHeight;
  }

  // 論理座標 → 物理バッファインデックス (MADCTL MV/MX/MY 適用)
  uint32_t physIndex(uint32_t lcol, uint32_t lrow) const {
    bool mv = (madctl >> 5) & 1;
    bool mx = (madctl >> 6) & 1;
    bool my = (madctl >> 7) & 1;
    uint32_t px = mv ? lrow : lcol;
    uint32_t py = mv ? lcol : lrow;
    if (mx) px = config.lcdWidth - 1 - px;
    if (my) py = config.lcdHeight - 1 - py;
    return py * config.lcdWidth + px;
  }

  // MADCTL 変更・RAMWR 開始時にキャッシュを更新する
  void updateWriteCache() {
    bool mv = (madctl >> 5) & 1;
    bool mx = (madctl >> 6) & 1;
    bool my = (madctl >> 7) & 1;
    cachedBGR = (madctl >> 3) & 1;
    int32_t W = static_cast<int32_t>(config.lcdWidth);
    if (!mv) {
      cachedHStep = mx ? -1 : +1;
    } else {
      cachedHStep = my ? -W : +W;
    }
    if (framebuf) writePtr = framebuf + physIndex(ramwrX, ramwrY);
  }

  void calcScaleParams() {
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

  void log(const char* msg) const {
    if (host.log) host.log(host.userData, msg);
  }

  void softReset() {
    sleeping = true;
    displayOn = false;
    inverted = false;
    pixelFormat = PixelFormat::RGB565;
    madctl = 0;
    currentCmd = CMD_NOP;
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

  // --- ピクセル書き込み ---

  // RGB565 値を 1 ピクセルとしてフレームバッファに書く (MADCTL BGR 考慮)
  [[gnu::always_inline]] void writePixelRgb565(uint16_t px) {
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
  void processRamwrData(const uint8_t* data, size_t length) {
    size_t i = 0;

    switch (pixelFormat) {
      case PixelFormat::RGB565:
        // 残余 1 バイトの drain
        if (ramwrBufLen == 1 && i < length) {
          writePixelRgb565(
              static_cast<uint16_t>((ramwrBuf[0] << 8) | data[i++]));
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
        // byte0: R1[3:0] G1[3:0]  byte1: B1[3:0] R2[3:0]  byte2: G2[3:0]
        // B2[3:0] 4bit → 5bit: (x<<1)|(x>>3)  4bit → 6bit: (x<<2)|(x>>2) 残余
        // (0〜2 バイト) の drain
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
        // R5 = byte0>>3, G6 = byte1>>2, B5 = byte2>>3 で直接 RGB565
        // に変換できる 残余 (0〜2 バイト) の drain
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

  // --- コマンド処理 ---

  void dispatchCommand(uint8_t cmd) {
    currentCmd = cmd;
    cmdDataLen = 0;
    ramwrBufLen = 0;

    switch (cmd) {
      case CMD_NOP: break;
      case CMD_SWRESET:
        softReset();
        log("SWRESET");
        break;
      case CMD_SLPOUT:
        sleeping = false;
        log("SLPOUT");
        break;
      case CMD_INVOFF:
        inverted = config.invertInvPolarity;
        log("INVOFF");
        break;
      case CMD_INVON:
        inverted = !config.invertInvPolarity;
        log("INVON");
        break;
      case CMD_DISPON:
        displayOn = true;
        log("DISPON");
        break;
      case CMD_RAMWR:
        ramwrX = casetXS;
        ramwrY = rasetYS;
        ramwrBufLen = 0;
        updateWriteCache();
        break;
      // CMD_MADCTL / CMD_COLMOD / CMD_CASET / CMD_RASET はデータバイトを待つ
      default: break;
    }
  }

  // CMD_MADCTL / CMD_COLMOD など RAMWR 以外のデータバイト処理
  // (呼び出し頻度は低い)
  void feedDataByte(uint8_t byte) {
    switch (currentCmd) {
      case CMD_MADCTL:
        if (cmdDataLen == 0) {
          madctl = byte;
          updateWriteCache();
          log("MADCTL");
        }
        ++cmdDataLen;
        break;
      case CMD_COLMOD:
        if (cmdDataLen == 0) {
          uint8_t fmt = byte & 0x07;
          if (fmt == 0x03)
            pixelFormat = PixelFormat::RGB444;
          else if (fmt == 0x05)
            pixelFormat = PixelFormat::RGB565;
          else if (fmt == 0x06)
            pixelFormat = PixelFormat::RGB666;
          ramwrBufLen = 0;  // フォーマット変更でバッファリセット
          log("COLMOD");
        }
        ++cmdDataLen;
        break;
      case CMD_CASET:
        switch (cmdDataLen) {
          case 0: ramwrBuf[0] = byte; break;
          case 1:
            casetXS = static_cast<uint16_t>((ramwrBuf[0] << 8) | byte);
            break;
          case 2: ramwrBuf[0] = byte; break;
          case 3:
            casetXE = static_cast<uint16_t>((ramwrBuf[0] << 8) | byte);
            log("CASET");
            break;
          default: break;
        }
        ++cmdDataLen;
        break;
      case CMD_RASET:
        switch (cmdDataLen) {
          case 0: ramwrBuf[0] = byte; break;
          case 1:
            rasetYS = static_cast<uint16_t>((ramwrBuf[0] << 8) | byte);
            break;
          case 2: ramwrBuf[0] = byte; break;
          case 3:
            rasetYE = static_cast<uint16_t>((ramwrBuf[0] << 8) | byte);
            log("RASET");
            break;
          default: break;
        }
        ++cmdDataLen;
        break;
      default: break;
    }
  }

  // データバイト列の処理: RAMWR は一括、それ以外は 1 バイトずつ
  [[gnu::always_inline]] void feedData(const uint8_t* data, size_t length) {
    if (currentCmd == CMD_RAMWR) {
      processRamwrData(data, length);
    } else {
      for (size_t i = 0; i < length; ++i) feedDataByte(data[i]);
    }
  }
};

//=============================================================================
// calcRequiredMemory
//=============================================================================
size_t calcRequiredMemory(const Sl2dConfig& config) {
  size_t fbSize = (size_t)config.lcdWidth * config.lcdHeight * sizeof(uint16_t);
  return sizeof(Impl) + fbSize;
}

//=============================================================================
// SpiLcd2Dvi
//=============================================================================
SpiLcd2Dvi::SpiLcd2Dvi(const Sl2dConfig& config, const HostInterface& host)
    : impl_(nullptr) {
  if (!host.alloc || !host.free) return;

  void* mem = host.alloc(sizeof(Impl));
  if (!mem) return;

  Impl* p = static_cast<Impl*>(mem);
  memset(p, 0, sizeof(Impl));
  p->config = config;
  p->host = host;
  p->status = Status::OK;

  if (config.lcdWidth == 0 || config.lcdHeight == 0 || config.dviWidth == 0 ||
      config.dviHeight == 0) {
    p->status = Status::INVALID_PARAM;
    impl_ = mem;
    return;
  }

  size_t fbSize = (size_t)config.lcdWidth * config.lcdHeight * sizeof(uint16_t);
  p->framebuf = static_cast<uint16_t*>(host.alloc(fbSize));
  if (!p->framebuf) {
    p->status = Status::OUT_OF_MEMORY;
    impl_ = mem;
    return;
  }

  impl_ = mem;
  p->calcScaleParams();
  p->softReset();
}

SpiLcd2Dvi::~SpiLcd2Dvi() {
  if (!impl_) return;
  Impl* p = static_cast<Impl*>(impl_);
  if (p->framebuf) p->host.free(p->framebuf);
  p->host.free(impl_);
}

Status SpiLcd2Dvi::getStatus() const {
  if (!impl_) return Status::OUT_OF_MEMORY;
  return static_cast<const Impl*>(impl_)->status;
}

void SpiLcd2Dvi::inputReset(bool assert) {
  if (!impl_) return;
  Impl* p = static_cast<Impl*>(impl_);
  if (p->status != Status::OK) return;
  p->hwReset = assert;
  if (!assert) {
    p->softReset();
    p->log("HW RESET released");
  }
}

void SpiLcd2Dvi::inputCommand(uint8_t byte) {
  if (!impl_) return;
  Impl* p = static_cast<Impl*>(impl_);
  if (p->status != Status::OK || p->hwReset) return;
  p->dispatchCommand(byte);
}

void SpiLcd2Dvi::inputData(const uint8_t* data, size_t length) {
  if (!impl_) return;
  Impl* p = static_cast<Impl*>(impl_);
  if (p->status != Status::OK || p->hwReset) return;
  p->feedData(data, length);
}

void SpiLcd2Dvi::fillScanline(uint16_t dviLine, uint16_t* dst) const {
  if (!impl_) return;
  Impl* p = static_cast<Impl*>(impl_);
  if (p->status != Status::OK || !p->framebuf) return;

  const uint16_t dviW = p->config.dviWidth;
  const uint16_t lcdW = p->config.lcdWidth;

  // 表示オフ・スリープ中、または垂直方向の黒帯
  if (p->sleeping || !p->displayOn || dviLine < p->displayY ||
      dviLine >= p->displayY + p->displayH) {
    memset(dst, 0, dviW * sizeof(uint16_t));
    return;
  }

  // 垂直マッピング: 固定小数点乗算で LCD 行を求める
  uint16_t lcdRow = static_cast<uint16_t>(
      ((uint32_t)(dviLine - p->displayY) * p->vStep) >> 16);
  const uint16_t* srcRow = p->framebuf + (uint32_t)lcdRow * lcdW;

  uint16_t* d = dst;

  // 左の黒帯
  memset(d, 0, (size_t)p->displayX * sizeof(uint16_t));
  d += p->displayX;

  // アクティブ領域: 水平スケーリング + 輝度反転
  // 反転: RGB565 全ビット XOR → (31-R, 63-G, 31-B) で各チャネル反転
  uint32_t hAccum = 0;
  if (p->inverted) {
    for (uint16_t x = 0; x < p->displayW; ++x) {
      *d++ = srcRow[hAccum >> 16] ^ 0xFFFFu;
      hAccum += p->hStep;
    }
  } else {
    for (uint16_t x = 0; x < p->displayW; ++x) {
      *d++ = srcRow[hAccum >> 16];
      hAccum += p->hStep;
    }
  }

  // 右の黒帯
  memset(d, 0, (size_t)(dviW - p->displayX - p->displayW) * sizeof(uint16_t));
}

uint16_t* SpiLcd2Dvi::getFramebuf() {
  if (!impl_) return nullptr;
  return static_cast<Impl*>(impl_)->framebuf;
}

void SpiLcd2Dvi::setDisplayOn(bool on) {
  if (!impl_) return;
  Impl* p = static_cast<Impl*>(impl_);
  if (p->status != Status::OK) return;
  p->sleeping = !on;
  p->displayOn = on;
}

}  // namespace sl2d
