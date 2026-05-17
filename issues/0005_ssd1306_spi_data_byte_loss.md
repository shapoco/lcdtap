# SSD1306 4-Line SPI でデータバイトが欠落し水平方向にズレが生じる

## 現象

`pico2_universal` で SSD1306 を 4-Line SPI + Horizontal Addressing Mode で使用すると、フレームバッファに書き込まれる画像が page ごとに数ピクセルずつ水平方向にズレていく。

- SPI クロック 8 MHz (平均 600 kHz 程度) での発生を確認。
- ST7789 + 4-Line SPI では同様の現象は起きない。
- SSD1306 + I2C モードでも起きない。

## 解析

`Ssd1306Controller::processRamwrData` のループ境界が基底クラス `ControllerBase::processRamwrData` と異なる規約になっていた。

基底クラスは `numBytes`（要素数）を受け取り `length = numBytes * stride` に変換してからバイトオフセットのループ上限に使うが、SSD1306 の override は変換を行わず `numBytes` をそのまま上限に使っていた。

```cpp
// 基底クラス (lib/src/lcdtap.cpp)
int32_t length = numBytes * stride;          // 要素数 → バイト数に変換
for (int32_t i = 0; i < length; i += stride) { ... }

// SSD1306 override の誤り (lib/src/ssd1306_controller.cpp)
for (uint32_t i = 0; i < length; i += stride) { ... }  // length = numBytes のまま
```

`stride = sizeof(uint32_t) = 4` のとき、ループが `numBytes / stride` 回しか回らず、バッチサイズが 2 以上になると最大 75% のバイトが処理されない。

`processSpiRingBuf()` は DVI スキャンライン同期で高頻度に呼ばれ、1 回あたりのバッチは平均 1〜2 ワード程度。バッチサイズ 2〜4 で 1〜3 バイトが欠落する。Horizontal Addressing Mode では page 間にアドレスリセットコマンドが来ないため、欠落が累積して page ごとに数ピクセルずつのズレとして現れる。

ST7789 では基底クラスの override を使うため正常。I2C では受信レートが低くバッチサイズがほぼ 1 になるためバグが顕在化しない。

## 対応

`Ssd1306Controller::processRamwrData` に基底クラスと同じ変換を追加した。

```cpp
// lib/src/ssd1306_controller.cpp
const uint32_t byteLen = numElems * stride;
for (uint32_t i = 0; i < byteLen; i += stride) { ... }
```
