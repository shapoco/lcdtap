#include <spilcd2dvi/spilcd2dvi.hpp>

#include <cstring>

namespace sl2d {

//=============================================================================
// DVI タイミングプリセット
//=============================================================================
namespace timing {

const DviTiming VGA_640X480_60HZ = {
    25175,
    {640, 16, 96, 48, false},
    {480, 10, 2, 33, false},
};

const DviTiming SVGA_800X600_60HZ = {
    40000,
    {800, 40, 128, 88, true},
    {600, 1, 4, 23, true},
};

const DviTiming HD_720P_60HZ = {
    74250,
    {1280, 110, 40, 220, true},
    {720, 5, 5, 20, true},
};

} // namespace timing

//=============================================================================
// コマンドコード
//=============================================================================
static constexpr uint8_t CMD_NOP     = 0x00;
static constexpr uint8_t CMD_SWRESET = 0x01;
static constexpr uint8_t CMD_SLPOUT  = 0x11;
static constexpr uint8_t CMD_INVOFF  = 0x20;
static constexpr uint8_t CMD_INVON   = 0x21;
static constexpr uint8_t CMD_DISPON  = 0x29;
static constexpr uint8_t CMD_RAMWR   = 0x2C;
static constexpr uint8_t CMD_MADCTL  = 0x36;
static constexpr uint8_t CMD_COLMOD  = 0x3A;

//=============================================================================
// 内部実装構造体 (PIMPL)
//=============================================================================
struct Impl {
    Sl2dConfig    config;
    HostInterface host;
    Status        status;
    bool          hwReset;

    uint8_t* framebuf;    // lcdWidth × lcdHeight × 3 バイトの RGB888 フレームバッファ
    uint8_t* scanlineBuf; // dviTiming.h.active × 3 バイトの出力ラインバッファ

    // ST7789 レジスタ
    bool        sleeping;
    bool        displayOn;
    bool        inverted;
    PixelFormat pixelFormat;
    uint8_t     madctl;

    // コマンドステートマシン
    uint8_t  currentCmd;
    uint8_t  cmdDataLen;  // 現コマンドで受け取ったデータバイト数

    // RAMWR 状態
    uint32_t ramwrPos;    // フレームバッファ内の書き込み位置 (ピクセル単位)
    uint8_t  ramwrBuf[3]; // 半端バイト蓄積 (最大 3 バイト)
    uint8_t  ramwrBufLen;

    // スケーリングパラメータ (コンストラクタで計算済み)
    uint16_t displayX;  // DVI上のLCD表示領域 開始X座標
    uint16_t displayY;  // DVI上のLCD表示領域 開始Y座標
    uint16_t displayW;  // DVI上のLCD表示領域の幅
    uint16_t displayH;  // DVI上のLCD表示領域の高さ
    uint32_t hStep;     // 水平固定小数点ステップ (16.16形式: lcdW<<16 / displayW)

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
        if (mx) px = config.lcdWidth  - 1 - px;
        if (my) py = config.lcdHeight - 1 - py;
        return py * config.lcdWidth + px;
    }

    void calcScaleParams() {
        uint16_t dviW = config.dviTiming.h.active;
        uint16_t dviH = config.dviTiming.v.active;
        uint16_t lcdW = config.lcdWidth;
        uint16_t lcdH = config.lcdHeight;

        switch (config.scaleMode) {
        case ScaleMode::STRETCH:
            displayX = 0; displayY = 0;
            displayW = dviW; displayH = dviH;
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
            uint16_t scale  = scaleH < scaleV ? scaleH : scaleV;
            if (scale == 0) scale = 1;
            displayW = lcdW * scale;
            displayH = lcdH * scale;
            displayX = (dviW - displayW) / 2;
            displayY = (dviH - displayH) / 2;
            break;
        }
        }

        hStep = ((uint32_t)lcdW << 16) / displayW;
    }

    void log(const char* msg) const {
        if (host.log) host.log(host.userData, msg);
    }

    void softReset() {
        sleeping    = true;
        displayOn   = false;
        inverted    = false;
        pixelFormat = PixelFormat::RGB565;
        madctl      = 0;
        currentCmd  = CMD_NOP;
        cmdDataLen  = 0;
        ramwrPos    = 0;
        ramwrBufLen = 0;
        memset(framebuf, 0, (size_t)config.lcdWidth * config.lcdHeight * 3);
    }

    // --- ピクセル書き込み ---

    // RGB888 チャンネル値で 1 ピクセルをフレームバッファに書く
    void writePixel(uint8_t r, uint8_t g, uint8_t b) {
        // MADCTL BGR: R と B を入れ替える
        if ((madctl >> 3) & 1) {
            uint8_t tmp = r; r = b; b = tmp;
        }

        uint32_t lw    = logicalWidth();
        uint32_t total = lw * logicalHeight();
        uint32_t idx   = physIndex(ramwrPos % lw, ramwrPos / lw) * 3;
        framebuf[idx    ] = r;
        framebuf[idx + 1] = g;
        framebuf[idx + 2] = b;
        if (++ramwrPos >= total) ramwrPos = 0;
    }

    // RAMWR の 1 バイトを処理し、ピクセルが揃ったら書き込む
    void processRamwrByte(uint8_t byte) {
        ramwrBuf[ramwrBufLen++] = byte;

        switch (pixelFormat) {
        case PixelFormat::RGB444:
            // 3 バイトで 2 ピクセル (各 4bit × RGB)
            // byte0: R1[3:0] G1[3:0]
            // byte1: B1[3:0] R2[3:0]
            // byte2: G2[3:0] B2[3:0]
            if (ramwrBufLen == 3) {
                uint8_t r1 = ramwrBuf[0] >> 4, g1 = ramwrBuf[0] & 0x0F;
                uint8_t b1 = ramwrBuf[1] >> 4, r2 = ramwrBuf[1] & 0x0F;
                uint8_t g2 = ramwrBuf[2] >> 4, b2 = ramwrBuf[2] & 0x0F;
                // 4bit → 8bit: 上位ビットを下位へ複製して拡張
                writePixel((r1 << 4) | r1, (g1 << 4) | g1, (b1 << 4) | b1);
                writePixel((r2 << 4) | r2, (g2 << 4) | g2, (b2 << 4) | b2);
                ramwrBufLen = 0;
            }
            break;

        case PixelFormat::RGB565:
            // 2 バイトで 1 ピクセル (ビッグエンディアン)
            if (ramwrBufLen == 2) {
                uint16_t px = static_cast<uint16_t>((ramwrBuf[0] << 8) | ramwrBuf[1]);
                uint8_t r = (px >> 11) & 0x1F;
                uint8_t g = (px >>  5) & 0x3F;
                uint8_t b = (px      ) & 0x1F;
                // 5/6bit → 8bit: 上位ビットを下位へ複製して拡張
                writePixel((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
                ramwrBufLen = 0;
            }
            break;

        case PixelFormat::RGB666:
            // 3 バイトで 1 ピクセル (各バイト上位 6bit が有効)
            if (ramwrBufLen == 3) {
                uint8_t r = ramwrBuf[0] >> 2;  // R[5:0]
                uint8_t g = ramwrBuf[1] >> 2;  // G[5:0]
                uint8_t b = ramwrBuf[2] >> 2;  // B[5:0]
                // 6bit → 8bit: 上位ビットを下位へ複製して拡張
                writePixel((r << 2) | (r >> 4), (g << 2) | (g >> 4), (b << 2) | (b >> 4));
                ramwrBufLen = 0;
            }
            break;
        }
    }

    // --- コマンド処理 ---

    void dispatchCommand(uint8_t cmd) {
        currentCmd  = cmd;
        cmdDataLen  = 0;
        ramwrBufLen = 0;

        switch (cmd) {
        case CMD_NOP:
            break;
        case CMD_SWRESET:
            softReset();
            log("SWRESET");
            break;
        case CMD_SLPOUT:
            sleeping = false;
            log("SLPOUT");
            break;
        case CMD_INVOFF:
            inverted = false;
            break;
        case CMD_INVON:
            inverted = true;
            break;
        case CMD_DISPON:
            displayOn = true;
            log("DISPON");
            break;
        case CMD_RAMWR:
            ramwrPos    = 0;
            ramwrBufLen = 0;
            break;
        // CMD_MADCTL / CMD_COLMOD はデータバイトを待つ
        default:
            break;
        }
    }

    void feedData(uint8_t byte) {
        switch (currentCmd) {
        case CMD_RAMWR:
            processRamwrByte(byte);
            break;
        case CMD_MADCTL:
            if (cmdDataLen == 0) {
                madctl = byte;
                log("MADCTL");
            }
            ++cmdDataLen;
            break;
        case CMD_COLMOD:
            if (cmdDataLen == 0) {
                uint8_t fmt = byte & 0x07;
                if (fmt == 0x03) pixelFormat = PixelFormat::RGB444;
                else if (fmt == 0x05) pixelFormat = PixelFormat::RGB565;
                else if (fmt == 0x06) pixelFormat = PixelFormat::RGB666;
                ramwrBufLen = 0;  // フォーマット変更でバッファリセット
                log("COLMOD");
            }
            ++cmdDataLen;
            break;
        default:
            break;
        }
    }
};

//=============================================================================
// calcRequiredMemory
//=============================================================================
size_t calcRequiredMemory(const Sl2dConfig& config) {
    size_t fbSize = (size_t)config.lcdWidth * config.lcdHeight * 3;
    size_t slSize = (size_t)config.dviTiming.h.active * 3;
    return sizeof(Impl) + fbSize + slSize;
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
    p->host   = host;
    p->status = Status::OK;

    if (config.lcdWidth == 0 || config.lcdHeight == 0 ||
        config.dviTiming.h.active == 0 || config.dviTiming.v.active == 0) {
        p->status = Status::INVALID_PARAM;
        impl_ = mem;
        return;
    }

    size_t fbSize = (size_t)config.lcdWidth * config.lcdHeight * 3;
    p->framebuf = static_cast<uint8_t*>(host.alloc(fbSize));
    if (!p->framebuf) {
        p->status = Status::OUT_OF_MEMORY;
        impl_ = mem;
        return;
    }

    size_t slSize = (size_t)config.dviTiming.h.active * 3;
    p->scanlineBuf = static_cast<uint8_t*>(host.alloc(slSize));
    if (!p->scanlineBuf) {
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
    if (p->scanlineBuf) p->host.free(p->scanlineBuf);
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
    for (size_t i = 0; i < length; ++i) {
        p->feedData(data[i]);
    }
}

const uint8_t* SpiLcd2Dvi::getScanline(uint16_t dviLine) const {
    if (!impl_) return nullptr;
    Impl* p = static_cast<Impl*>(impl_);  // scanlineBuf への書き込みが必要なため非 const
    if (p->status != Status::OK || !p->framebuf || !p->scanlineBuf) return nullptr;

    const uint16_t dviW = p->config.dviTiming.h.active;
    const uint16_t lcdW = p->config.lcdWidth;
    const uint16_t lcdH = p->config.lcdHeight;
    uint8_t*       dst  = p->scanlineBuf;

    // 表示オフ・スリープ中、または垂直方向の黒帯
    if (p->sleeping || !p->displayOn ||
        dviLine < p->displayY || dviLine >= p->displayY + p->displayH) {
        memset(dst, 0, dviW * 3);
        return dst;
    }

    // 垂直マッピング: DVI 行 → LCD 行
    uint16_t lcdRow = (uint32_t)(dviLine - p->displayY) * lcdH / p->displayH;
    const uint8_t* srcRow = p->framebuf + (uint32_t)lcdRow * lcdW * 3;

    uint8_t* d = dst;

    // 左の黒帯
    memset(d, 0, (size_t)p->displayX * 3);
    d += p->displayX * 3;

    // アクティブ領域: 水平スケーリング + 輝度反転
    uint32_t hAccum = 0;
    if (p->inverted) {
        for (uint16_t x = 0; x < p->displayW; ++x) {
            const uint8_t* src = srcRow + (hAccum >> 16) * 3;
            *d++ = src[0] ^ 0xFF;
            *d++ = src[1] ^ 0xFF;
            *d++ = src[2] ^ 0xFF;
            hAccum += p->hStep;
        }
    } else {
        for (uint16_t x = 0; x < p->displayW; ++x) {
            const uint8_t* src = srcRow + (hAccum >> 16) * 3;
            *d++ = src[0];
            *d++ = src[1];
            *d++ = src[2];
            hAccum += p->hStep;
        }
    }

    // 右の黒帯
    memset(d, 0, (size_t)(dviW - p->displayX - p->displayW) * 3);

    return dst;
}

} // namespace sl2d
