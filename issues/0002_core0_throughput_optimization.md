# Core0 スループット最適化

## 問題

SPI 入力が 60Hz フレームレートの場合に DVI 出力が全面赤になる。
Core0 が DVI スキャンライン生成と SPI リングバッファのドレインを同時に担っており、処理が追いつかないことが原因と判断した。

## 経緯

- 最適化 1〜5 を実施した結果、全面赤は解消しほぼ半分の内容が読み取れる程度まで改善した。
- 最適化 6 を実施した結果、40〜50 FPS 程度まで綺麗に表示できるようになった。
- 60 FPS での動作確認は別途実施予定。

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

`writePixelRgb565` / `feedData` にインライン展開を強制し、関数呼び出しのオーバーヘッドを排除した。

### 5. `inputData` ホットループの再構築

**変更前**: 1 バイトごとに `feedData(byte)` → `switch(currentCmd)` → `processRamwrByte(byte)` → `switch(pixelFormat)` の呼び出し連鎖を繰り返していた。

**変更後**: `currentCmd` と `pixelFormat` の switch をループ外に追い出した。

```
【変更前】
inputData(data, len)
  for i in 0..len-1:
    feedData(data[i])         ← per-byte
      switch(currentCmd)      ← per-byte
        processRamwrByte()    ← per-byte
          switch(pixelFormat) ← per-byte

【変更後】
inputData(data, len)
  feedData(data, len)
    switch(currentCmd)        ← 一度だけ
      CMD_RAMWR →
        processRamwrData(data, len)
          switch(pixelFormat) ← 一度だけ
            タイトループ
```

`processRamwrData` は各ピクセルフォーマットごとに:
1. 前回呼び出しからの端数バイト (`ramwrBuf`) を drain
2. 揃ったバイトをタイトループで一括処理
3. 残った端数バイトを `ramwrBuf` に保存

RGB565 のタイトループ（正常系）:
```cpp
while (i + 2 <= length) {
    writePixelRgb565((data[i] << 8) | data[i + 1]);
    i += 2;
}
```

また、`example/pico2/src/main.cpp` において以下の変更も行った（ユーザーによる）:
- `process_spi_ring_buf()` の予備的な呼び出しを削除し、DVI キューへの応答レイテンシを削減
- `flush_data_batch` のインライン化と `g_sl2d` のヌルチェックを冒頭に集約

**効果**: `switch` の評価をデータバイト数分から 1 回に削減。

### 6. RGB444 / RGB666 の直接 RGB565 パック

**変更前**: `writePixel(r8, g8, b8)` を経由していたため、4→8bit / 6→8bit への無駄な展開が発生していた。また `writePixel` 内と `writePixelRgb565` 内の BGR swap が打ち消し合い、RGB444/666 では BGR モードが実質無効になっていた（バグ）。

**変更後**: `writePixel` を削除し、`processRamwrData` から `writePixelRgb565` を直接呼ぶ。

RGB444（4bit チャンネル）から RGB565 への直接変換:
- R5/B5: `(x<<1)|(x>>3)` (上位ビット繰り返し)
- G6: `(x<<2)|(x>>2)` (上位 2bit 繰り返し)

RGB666（各バイト上位 6bit 有効）から RGB565 への直接変換:
- `(byte0 & 0xF8) << 8 | (byte1 & 0xFC) << 3 | byte2 >> 3` の 1 式で完結

**挙動の変化（意図的）**: RGB444/666 で BGR モードが正しく機能するようになった。

## 変更ファイル

- `lib/include/spilcd2dvi/spilcd2dvi.hpp`: `getScanline` → `fillScanline(line, dst)` に API 変更
- `lib/src/spilcd2dvi.cpp`: 上記最適化の実装
- `example/pico2/src/main.cpp`: `fillScanline` への呼び出し変更、`process_spi_ring_buf` の改善

## 検証

実機では 40〜50 FPS 程度まで綺麗に表示できることを確認。60 FPS での動作確認は別途実施予定。
