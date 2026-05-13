#pragma once

#include <cstddef>
#include <lcdtap/lcdtap.hpp>

namespace lcdtap {

// ControllerBase — 共通内部実装基底クラス (PIMPL 実体)
//
// フレームバッファを持ち、矩形アドレッシング (CASET/RASET) + RAMWR 方式で
// 制御されるディスプレイコントローラ共通の状態と処理を保持する。
// コントローラ固有の部分 (コマンドコード・MADCTL 等) は派生クラスで実装する。
class ControllerBase {
 public:
  virtual ~ControllerBase() = default;

  // --- 共通設定・状態 ---
  LcdTapConfig config;
  HostInterface host;
  Status status;
  bool hwReset;

  uint16_t* framebuf;  // lcdWidth × lcdHeight × sizeof(uint16_t) の RGB565

  // 表示制御状態
  bool sleeping;
  bool displayOn;
  bool inverted;
  PixelFormat pixelFormat;

  // コマンドステートマシン
  uint8_t currentCmd;
  uint8_t cmdDataLen;  // 現コマンドで受け取ったデータバイト数

  // RAMWR アドレッシング
  uint16_t casetXS;  // CASET 開始列 (論理座標)
  uint16_t casetXE;  // CASET 終了列 (論理座標、inclusive)
  uint16_t rasetYS;  // RASET 開始行 (論理座標)
  uint16_t rasetYE;  // RASET 終了行 (論理座標、inclusive)
  uint16_t ramwrX;   // 現在の書き込み位置 X (論理座標)
  uint16_t ramwrY;   // 現在の書き込み位置 Y (論理座標)
  uint8_t
      ramwrBuf[3];  // 半端バイト蓄積 / コマンドデータ一時格納 (最大 3 バイト)
  uint8_t ramwrBufLen;

  // スケーリングパラメータ (コンストラクタで計算済み)
  uint16_t displayX;  // DVI 上の LCD 表示領域 開始 X 座標
  uint16_t displayY;  // DVI 上の LCD 表示領域 開始 Y 座標
  uint16_t displayW;  // DVI 上の LCD 表示領域の幅
  uint16_t displayH;  // DVI 上の LCD 表示領域の高さ
  uint32_t hStep;  // 水平固定小数点ステップ (16.16 形式: lcdW<<16 / displayW)
  uint32_t vStep;  // 垂直固定小数点ステップ (16.16 形式: lcdH<<16 / displayH)
  uint8_t outputRotation;  // 出力回転 0..3 (0:なし, 1:90°CW, 2:180°, 3:270°CW)

  // RAMWR 書き込みキャッシュ (updateWriteCache() で更新)
  bool cachedBGR;
  int32_t cachedHOffset, cachedHStep;
  int32_t cachedVOffset, cachedVStep;
  uint16_t* writePtr;

  // --- コントローラ固有の virtual インタフェース ---

  // MADCTL MV を考慮した論理幅/高さ
  virtual uint16_t logicalWidth() const = 0;
  virtual uint16_t logicalHeight() const = 0;

  // 論理座標 → 物理バッファインデックス
  [[gnu::always_inline]] uint32_t physIndex(uint32_t lcol, uint32_t lrow) {
    return lcol * cachedHStep + cachedHOffset + lrow * cachedVStep +
           cachedVOffset;
  }

  // MADCTL 変更・RAMWR 開始時にキャッシュを更新する
  virtual void updateWriteCache() = 0;

  // ソフトリセット (コントローラ固有レジスタを初期化してから resetCommon()
  // を呼ぶ)
  virtual void softReset() = 0;

  // コマンドバイト処理
  virtual void dispatchCommand(uint8_t cmd) = 0;

  // RAMWR 以外のコマンドデータバイト処理
  virtual void feedDataByte(uint8_t byte) = 0;

  // 現コマンドが RAM 書き込み (RAMWR 相当) かどうか
  virtual bool isRamWriteCommand() const = 0;

  // --- 共通実装 ---

  // スケーリングパラメータの計算 (コンストラクタ内で呼ぶ)
  void calcScaleParams();

  // ログ出力ヘルパー
  void log(const char* msg) const;

  // 共通フィールドのリセット (派生クラスの softReset() から呼ぶ)
  void resetCommon();

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
  // 派生クラスで独自フォーマットを処理する場合はオーバーライドする
  virtual void processRamwrData(const uint8_t* data, uint32_t numBytes, uint32_t stride);

  // データバイト列の処理: RAMWR は一括、それ以外は 1 バイトずつ
  void feedData(const uint8_t* data, uint32_t numBytes, uint32_t stride = 1);
};

}  // namespace lcdtap
