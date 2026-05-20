# 0008: SETREMAP/MADCTL MV=1 設定後のフレームバッファ境界外書き込み

## 症状

SSD1331 で `display.setRotation(1)` を呼び出した後に `display.show()` などで
ピクセルデータを送信すると LcdTap が panic する (Pico ハードフォルト)。

- `setRotation(1)` のみ: 問題なし
- `setRotation(1)` + ピクセルデータ: 必ず panic

ST7789 でも MADCTL に MV ビット (bit 5) が立った状態で RAMWR を送ると
同様の境界外書き込みが発生する可能性がある。

## 原因

### コントローラ共通の設計

`writePixelRgb565()` はフレームバッファ書き込み位置を以下の式で管理する:

```
physIndex(lcol, lrow) = lcol * cachedHStep + cachedHOffset
                      + lrow * cachedVStep + cachedVOffset
```

`SETREMAP(REMAP_ADDR_INC=1)` / `MADCTL(MV=1)` が設定されると **縦方向アドレスインクリメント**
モードになり、ステップ値が入れ替わる:

| モード | cachedHStep | cachedVStep |
|--------|-------------|-------------|
| mv=0 (横) | ±1 | ±lcdWidth |
| mv=1 (縦) | ±lcdWidth | ±1 |

### バグの発生

`SETCOLUMN` / `CASET` は常にハードウェアカラム座標 (col 0..lcdWidth-1) を送ってくるが、
`casetXS/XE` にそのまま格納され、`physIndex()` のロジカル X として使われていた。

mv=1 のとき `cachedHStep = lcdWidth` なので:

```
physIndex(x = casetXE, y = 0)
  = casetXE × lcdWidth + ...
```

例: SSD1331 (96×64), SETCOLUMN(0,95), SETREMAP(REMAP_ADDR_INC=1)

```
physIndex(95, 0) = 95 × 96 = 9120
framebuf サイズ   = 96 × 64 = 6144  → 境界外!
```

例: ST7789 (320×240 landscape), CASET(0,319), MADCTL(MV=1)

```
physIndex(319, 0) = 319 × 320 = 102080
framebuf サイズ   = 320 × 240 = 76800  → 境界外!
```

## 修正 (commit: TBD)

### 修正方針

`SETCOLUMN` / `CASET` および `SETROW` / `RASET` の値を**ハードウェア座標**として
別フィールド (`hwColStart/End`, `hwRowStart/End`) に保存し、
`updateWriteCache()` 内で mv フラグに応じてロジカル座標に変換してから
`casetXS/XE` / `rasetYS/YE` に格納する。

| mv | ロジカル X (fast axis) | ロジカル Y (slow axis) |
|----|----------------------|----------------------|
| 0  | ハードウェア col      | ハードウェア row      |
| 1  | ハードウェア row      | ハードウェア col      |

### 検証

mv=1, mx=0, my=0 のとき:
- `cachedHStep = lcdWidth`, `cachedVStep = 1`
- `physIndex(hwRow, hwCol) = hwRow × lcdWidth + hwCol`
- 最大値: `physIndex(lcdHeight-1, lcdWidth-1) = (lcdHeight-1) × lcdWidth + (lcdWidth-1) = lcdWidth × lcdHeight - 1` ✓

### 変更ファイル

- `lib/src/ssd1331_controller.hpp` / `.cpp`: `hwColStart/End`, `hwRowStart/End` 追加
- `lib/src/st7789_controller.hpp` / `.cpp`: 同上 (ST7789 は RAMWR ハンドラも変更)

### ST7789 RAMWR ハンドラの変更

ST7789 は `dispatchCommand(CMD_RAMWR)` で `ramwrX = casetXS` を設定してから
`updateWriteCache()` を呼んでいたため、casetXS がハードウェア座標のまま
ramwrX に入ってしまっていた。修正後は `updateWriteCache()` を先に呼んで
hw→logical 変換を済ませてから `ramwrX = casetXS` を設定する。

## 影響範囲

| コントローラ | 影響 |
|------------|------|
| SSD1331 | SETREMAP REMAP_ADDR_INC=1 設定後に panic → **修正済み** |
| ST7789  | MADCTL MV=1 かつ lcdWidth > lcdHeight のとき境界外書き込み → **修正済み** |
| SSD1306 | `cachedHStep` 固定値 1、MV 相当ビットなし → **影響なし** |
