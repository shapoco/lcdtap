#pragma once

// LcdTap — Core Library
// Interprets LCD controller commands and generates DVI-D signal content.
//
// Usage:
//   #include <lcdtap/lcdtap.hpp>

#include <cstddef>
#include <cstdint>

namespace lcdtap {

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
// LCDコントローラの種類
//=============================================================================
enum class ControllerType : uint8_t {
  ST7789,
  SSD1309,
};

//=============================================================================
// ピクセルフォーマット (SPI 入力側 — COLMOD 相当)
//=============================================================================
enum class PixelFormat : uint8_t {
  MONO_VPACK = 0x00,  // 1bpp モノクロ縦8ピクセルパック (SSD1309)
  RGB444 = 0x03,      // 12bpp
  RGB565 = 0x05,      // 16bpp
  RGB666 = 0x06,      // 18bpp
};

//=============================================================================
// スケーリングモード (LCD 解像度 ≠ DVI アクティブ領域の場合)
//=============================================================================
enum class ScaleMode : uint8_t {
  STRETCH,  // DVI 全体に引き伸ばす (アスペクト比無視)
  FIT,  // アスペクト比を保ってレターボックス/ピラーボックス表示
  PIXEL_PERFECT,  // 整数倍で拡大、余白は黒
};

//=============================================================================
// 設定構造体
//=============================================================================
struct LcdTapConfig {
  // --- LCDコントローラ ---
  ControllerType controller;

  // --- SPI 入力 (LCD) 側 ---
  uint16_t lcdWidth;
  uint16_t lcdHeight;
  PixelFormat pixelFormat;  // 初期ピクセルフォーマット (COLMOD で変更可)

  // --- DVI 出力側 ---
  uint16_t dviWidth;   // DVI アクティブ領域の幅 (ピクセル)
  uint16_t dviHeight;  // DVI アクティブ領域の高さ (ライン)
  ScaleMode scaleMode;

  bool invertInvPolarity;  // true: INVON→非反転 / INVOFF→反転
};

//=============================================================================
// ホストインタフェース
//=============================================================================
struct HostInterface {
  // --- メモリ管理 (必須) ---
  // フレームバッファの確保/解放に使用する。
  // PSRAM 等の外部メモリに確保したい場合に独自のアロケータを渡す。
  void* (*alloc)(size_t size);
  void (*free)(void* ptr);

  // --- 通知コールバック (省略可、nullptr 可) ---

  // デバッグログ出力 (nullptr なら無効)
  void (*log)(void* userData, const char* message);

  void* userData;
};

//=============================================================================
// デフォルト設定の取得
// 指定したコントローラ向けのデフォルト値を cfg に書き込む。
// 必要に応じてフィールドを上書きして LcdTap のコンストラクタに渡す。
//=============================================================================
void getDefaultConfig(ControllerType type, LcdTapConfig* cfg);

//=============================================================================
// メインクラス
//=============================================================================
class LcdTap {
 public:
  LcdTap(const LcdTapConfig& config, const HostInterface& host);
  ~LcdTap();

  // コンストラクタ失敗時の状態確認
  Status getStatus() const;

  //--- SPI 入力 ---

  // ハードウェアリセット信号入力 (RESX ピン相当)
  // assert=true でリセット状態、false で解除
  void inputReset(bool assert);

  // コマンドバイト入力 (D/CX=Low)
  void inputCommand(uint8_t byte);

  // データバイト列入力 (D/CX=High)
  void inputData(const uint8_t* data, size_t length);

  //--- DVI 出力 ---

  // 指定ライン番号のピクセルデータを dst に書き込む。
  // フォーマット: RGB565 (1 ピクセル = uint16_t、R[15:11] G[10:5] B[4:0])
  // line の範囲: 0 .. dviHeight - 1 (DVI 座標系)
  // スケーリングはライブラリ内部で処理済み。
  // dst は dviWidth 個の uint16_t を格納できる領域を指すこと。
  void fillScanline(uint16_t line, uint16_t* dst) const;

  // ディスプレイ出力の回転設定。
  // rot=0: 従来通り (デフォルト)
  // rot=1: 時計回り 90° 回転。FIT/PIXEL_PERFECT ではアスペクト比が
  // 縦横入れ替わる。
  // rot=2: 上下左右反転。アスペクト比は変化しない。
  // rot=3: 時計回り 270° 回転。FIT/PIXEL_PERFECT
  // ではアスペクト比が縦横入れ替わる。
  // コントローラの内部状態には影響しない。
  // fillScanline の読み出しパターンのみ変わる。
  void setOutputRotation(int rot);

  //--- テスト / デバッグ用 ---

  // フレームバッファへの直接書き込みポインタを返す。
  // フォーマット: lcdWidth × lcdHeight × sizeof(uint16_t) バイト
  //              (行優先 RGB565、MADCTL 変換なし)。
  uint16_t* getFramebuf();

  // スリープ/表示オン状態を強制設定する。
  // on=true: sleeping=false かつ displayOn=true にして fillScanline()
  // を黒以外にする。 on=false: sleeping=true にして黒画面に戻す。 SPI
  // マスターから SLPOUT/DISPON
  // を受け取る前にテストパターンを表示したい場合に使用。
  void setDisplayOn(bool on);

 private:
  LcdTap(const LcdTap&) = delete;
  LcdTap& operator=(const LcdTap&) = delete;

  void* impl_;  // 内部実装を隠蔽 (PIMPL)
};

}  // namespace lcdtap
