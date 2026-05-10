# Core0 スループット最適化

## 問題

SPI 入力が 60Hz フレームレートの場合に DVI 出力が全面赤になる。
Core0 が DVI スキャンライン生成と SPI リングバッファのドレインを同時に担っており、処理が追いつかないことが原因と判断した。

## 実施した最適化

### 1. `fillScanline` — memcpy の排除

**変更前**: `getScanline()` が内部の `scanlineBuf` にデータを書いてポインタを返し、呼び出し側が `memcpy` で DVI バッファにコピーしていた。

**変更後**: `fillScanline(line, dst)` に API を変更し、DVI バッファ `dst` に直接書き込む形にした。これにより `scanlineBuf` の確保・解放も不要になり、メモリも削減できた。

**効果**: 640 × 2 byte × 480 lines × 60 fps ≈ 36 MB/s の SRAM 転送を削減。

### 2. `writePixelRgb565` — ピクセル毎の physIndex() 乗算を排除

**変更前**: RAMWR でのピクセル書き込みごとに `physIndex(ramwrX, ramwrY)` を呼び出していた。この関数は MADCTL の MV/MX/MY に基づく条件分岐 5 本と `py * lcdWidth` の乗算を含んでいた。

**変更後**: MADCTL 確定時（および RAMWR コマンド受信時）に `updateWriteCache()` を呼び、以下の値を事前計算してキャッシュする。

- `cachedBGR`: BGR スワップフラグ
- `cachedLogW` / `cachedLogH`: 論理幅・高さ
- `cachedHStep`: 1 ピクセル書き込むたびに `writePtr` に加算する差分（配列要素数単位）
- `cachedVLineStep`: 論理行の折り返し時に `writePtr` に加算する補正差分
- `writePtr`: 現在の書き込み位置へのポインタ

`writePixelRgb565` の書き込みは `*writePtr = px; writePtr += cachedHStep;` と比較のみになった。

MV/MX/MY 各組み合わせの `cachedHStep` / `cachedVLineStep`:

| MV | MX | MY | hStep  | vLineStep    |
|----|----|----|--------|--------------|
| 0  | 0  | 0  | +1     | 0            |
| 0  | 1  | 0  | -1     | +2·W         |
| 0  | 0  | 1  | +1     | -2·W         |
| 0  | 1  | 1  | -1     | 0            |
| 1  | 0  | 0  | +W     | 1 - W·H      |
| 1  | 1  | 0  | +W     | -1 - W·H     |
| 1  | 0  | 1  | -W     | 1 + W·H      |
| 1  | 1  | 1  | -W     | W·H - 1      |

（W = lcdWidth, H = lcdHeight）

**効果**: 240 × 320 × 60 fps ≈ 460 万回/秒の乗算と分岐を加算・比較のみに置き換え。

### 3. 垂直スケーリングの除算を固定小数点乗算に置き換え

**変更前**: `fillScanline` 内でスキャンラインごとに `(offset * lcdH) / displayH` の除算を実行していた。

**変更後**: `calcScaleParams()` で `vStep = (lcdH << 16) / displayH` を事前計算し、実行時は `(offset * vStep) >> 16` の乗算＋シフトに変更した。

### 4. ホットパス関数への `[[gnu::always_inline]]` 付与

`writePixelRgb565` / `writePixel` / `processRamwrByte` / `feedData` にインライン展開を強制し、関数呼び出しのオーバーヘッドを排除した。

## 変更ファイル

- `lib/include/spilcd2dvi/spilcd2dvi.hpp`: `getScanline` → `fillScanline(line, dst)` に API 変更
- `lib/src/spilcd2dvi.cpp`: 上記最適化の実装
- `example/pico2/src/main.cpp`: `fillScanline` への呼び出し変更

## 検証

ビルドは通過。実機での 60Hz 動作確認は別途実施予定。
