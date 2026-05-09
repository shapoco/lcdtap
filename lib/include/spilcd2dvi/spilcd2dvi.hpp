#pragma once

// SpiLcd2Dvi — Core Library
// Interprets SPI LCD (ST7789) commands and generates DVI-D signal content.
//
// Usage:
//   #include <spilcd2dvi/spilcd2dvi.hpp>

#include <cstddef>
#include <cstdint>

namespace sl2d {

//=============================================================================
// バージョン
//=============================================================================
inline constexpr int VERSION_MAJOR = 0;
inline constexpr int VERSION_MINOR = 1;
inline constexpr int VERSION_PATCH = 0;

//=============================================================================
// ステータスコード
//=============================================================================
enum class Status : int {
    OK = 0,
    INVALID_PARAM,  // config に不正な値がある
    OUT_OF_MEMORY,  // alloc() が nullptr を返した
    NOT_READY,      // コンストラクタ失敗後に操作を呼んだ
};

//=============================================================================
// ピクセルフォーマット (SPI 入力側 — ST7789 COLMOD 相当)
//=============================================================================
enum class PixelFormat : uint8_t {
    RGB444 = 0x03,  // 12bpp
    RGB565 = 0x05,  // 16bpp
    RGB666 = 0x06,  // 18bpp
};

//=============================================================================
// スケーリングモード (LCD 解像度 ≠ DVI アクティブ領域の場合)
//=============================================================================
enum class ScaleMode : uint8_t {
    STRETCH,       // DVI 全体に引き伸ばす (アスペクト比無視)
    FIT,           // アスペクト比を保ってレターボックス/ピラーボックス表示
    PIXEL_PERFECT, // 整数倍で拡大、余白は黒
};

//=============================================================================
// フレームバッファのピクセル型 (RGB 各 1 バイト)
//=============================================================================
struct Rgb888 {
    uint8_t r, g, b;
};

//=============================================================================
// DVI タイミング
//=============================================================================
struct DviTiming {
    uint32_t pixelClockKhz;

    struct Axis {
        uint16_t active;
        uint16_t frontPorch;
        uint16_t syncWidth;
        uint16_t backPorch;
        bool     syncPolarity; // true = positive
    } h, v;
};

// よく使うタイミングのプリセット
namespace timing {
    extern const DviTiming VGA_640X480_60HZ;    //  25.175 MHz
    extern const DviTiming SVGA_800X600_60HZ;   //  40.000 MHz
    extern const DviTiming HD_720P_60HZ;        //  74.250 MHz
} // namespace timing

//=============================================================================
// 設定構造体
//=============================================================================
struct Sl2dConfig {
    // --- SPI 入力 (LCD) 側 ---
    uint16_t    lcdWidth;
    uint16_t    lcdHeight;
    PixelFormat pixelFormat;  // ST7789 の COLMOD 設定に合わせる

    // --- DVI 出力側 ---
    DviTiming dviTiming;
    ScaleMode scaleMode;
};

//=============================================================================
// ホストインタフェース
//=============================================================================
struct HostInterface {
    // --- メモリ管理 (必須) ---
    // MCU 固有のメモリアーキテクチャに合わせた確保/解放関数を渡す。
    void* (*alloc)(size_t size);
    void  (*free)(void* ptr);

    // --- 通知コールバック (省略可、nullptr 可) ---

    // デバッグログ出力 (nullptr なら無効)
    void (*log)(void* userData, const char* message);

    void* userData;
};

//=============================================================================
// メモリ使用量の事前計算
// コンストラクタを呼ぶ前に必要メモリ量を確認するためのユーティリティ。
//=============================================================================
size_t calcRequiredMemory(const Sl2dConfig& config);

//=============================================================================
// メインクラス
//=============================================================================
class SpiLcd2Dvi {
public:
    SpiLcd2Dvi(const Sl2dConfig& config, const HostInterface& host);
    ~SpiLcd2Dvi();

    // コンストラクタ失敗時の状態確認
    Status getStatus() const;

    //--- SPI 入力 ---

    // ハードウェアリセット信号入力 (ST7789 の RESX ピン相当)
    // assert=true でリセット状態、false で解除
    void inputReset(bool assert);

    // コマンドバイト入力 (ST7789 の D/CX=Low)
    void inputCommand(uint8_t byte);

    // データバイト列入力 (ST7789 の D/CX=High)
    void inputData(const uint8_t* data, size_t length);

    //--- DVI 出力 ---

    // 指定ライン番号のピクセルデータへのポインタを返す。
    // フォーマット: RGB888 (1 ピクセル = R, G, B 各 1 バイトの計 3 バイト)
    // line の範囲: 0 .. dviTiming.v.active - 1 (DVI 座標系)
    // スケーリングはライブラリ内部で処理済み。
    // DVI ドライバから割り込みまたは DMA 転送の契機に呼び出す想定。
    const uint8_t* getScanline(uint16_t line) const;

private:
    SpiLcd2Dvi(const SpiLcd2Dvi&) = delete;
    SpiLcd2Dvi& operator=(const SpiLcd2Dvi&) = delete;

    void* impl_; // 内部実装を隠蔽 (PIMPL)
};

} // namespace sl2d
