# ピクセル処理スループット最適化

コミット: f0efa2897d22f265cd531905dffbb94c0b92c8c9

## 概要

クロック高速化（最大 62.5 MHz）に伴ってピクセル処理のスループットが不足したため、
以下の 2 点の最適化を実施した。

## 最適化 1: RGB444 ビット変換の簡略化

### 変更前

`writePixel(r8, g8, b8)` を経由して変換していた（4→8bit 展開を挟む）か、
あるいは以下の MSB 繰り返し方式を使用していた。

```
R5 = (r4 << 1) | (r4 >> 3)   // 5bit: 上位ビット繰り返し
G6 = (g4 << 2) | (g4 >> 2)   // 6bit: 上位 2bit 繰り返し
B5 = (b4 << 1) | (b4 >> 3)   // 5bit: 上位ビット繰り返し
```

この方式はチャンネルあたり 2 演算（シフト + OR）が必要だった。

### 変更後

`writePixelRgb565` を直接呼び出し、1 演算（シフト）で RGB565 フィールドに MSB アラインする
方式に変更した。

```cpp
// byte0: R1[3:0] G1[3:0]  byte1: B1[3:0] R2[3:0]  byte2: G2[3:0] B2[3:0]
// 4bit→5bit: x<<1 (MSB-align; LSB zeroed)
// 4bit→6bit: x<<2 (MSB-align; lower 2 bits zeroed)
pixel0 |= (b0 << 8) & 0xF000;  // R1[3:0] → RGB565 bits[15:12]
pixel0 |= (b0 << 7) & 0x0780;  // G1[3:0] → RGB565 bits[10:7]
pixel0 |= (b1 >> 3) & 0x001E;  // B1[3:0] → RGB565 bits[4:1]
```

変換誤差: 最大 1 LSB（値 0 → 0、値 15 → 30/62 ではなく最大値の 31/63 にならない）。
輝度精度より演算速度を優先した。

### 変更ファイル

- `lib/src/lcdtap.cpp`

## 最適化 2: ST7789 サンプルで DMA リングバッファを直接 inputData に渡す

### 変更前

SPI リングバッファから各バイトを取り出して別の中間バッファ（`dataBatch[]`）にコピーし、
その後 `inputData(dataBatch, len, 1)` を呼ぶ形だった。

### 変更後

中間バッファを廃止し、リングバッファのポインタを直接 `inputData` に渡す。
`inputData` の `stride` 引数に `sizeof(uint32_t)` を指定することで、
各 32bit ワードの低バイト（SPI データバイト）だけを読み飛ばしながら処理できるようにした。

```cpp
// コマンドバイトが来るたびに、それまでのデータ範囲を一括して渡す
gInst->inputData((uint8_t *)&spiRingBuf[dataStart], dataLen, sizeof(uint32_t));
```

リングバッファの各ワード: `[bit8: DCX, bits7:0: data byte]`
（上位 3 バイトは 0 またはフラグのみのため stride=4 で低バイトのみ読み取れる）

### 変更ファイル

- `example/pico2_st7789/src/main.cpp`

## 結果

40〜50 FPS 以上での安定動作を確認。
