# 0009: MV=1 時の CASET/RASET ウィンドウ X/Y 反転と範囲クリッピング誤り

## 症状

ST7789 / SSD1331 で MV=1 (行列交換モード) を設定した状態でピクセルを書き込むと
画像が崩れる。特に非正方形パネルで顕著。

## 原因

### CASET/RASET (ST7789) および SETCOLUMN/SETROW (SSD1331) の処理

issue #0008 の修正で `hwColStart/End` / `hwRowStart/End` フィールドと
`updateWriteCache()` 内の MV 対応スワップが追加された。
しかし CASET/RASET・SETCOLUMN/SETROW のハンドラは MV フラグに関わらず
常に hwCol / hwRow に固定で書き込んでいた。

`updateWriteCache()` MV=1 の動作:

```
casetXS = hwRowStart   // 論理 X (fast axis)
rasetYS = hwColStart   // 論理 Y (slow axis)
```

ホストドライバは **CASET を論理 X、RASET を論理 Y** として送る。
それにもかかわらず、ハンドラが CASET→hwCol、RASET→hwRow に格納するため:

- `casetXS` = hwRowStart = **RASET の値** → X に RASET 値が入る (誤り)
- `rasetYS` = hwColStart = **CASET の値** → Y に CASET 値が入る (誤り)

### クリッピング範囲の誤り

MV=1 のとき論理 X は lcdHeight 方向、論理 Y は lcdWidth 方向にマップされる。
しかし修正前は常に:

- CASET → `lcdWidth - 1` でクリップ (誤り。MV=1 では `lcdHeight - 1` が正しい)
- RASET → `lcdHeight - 1` でクリップ (誤り。MV=1 では `lcdWidth - 1` が正しい)

例: ST7789 (320×240 landscape, MV=1)
- ホストは CASET で 0〜319 を送るが `lcdWidth-1=239` でクリップ → 240〜319 が失われる

## 修正

### 修正方針

CASET/RASET および SETCOLUMN/SETROW のハンドラを MV フラグで分岐し、
格納先と上限値を切り替える。

| MV | コマンド | 格納先 | 上限 |
|----|---------|--------|------|
| 0 | CASET / SETCOLUMN | hwColStart/End | lcdWidth - 1  |
| 0 | RASET / SETROW    | hwRowStart/End | lcdHeight - 1 |
| 1 | CASET / SETCOLUMN | hwRowStart/End | lcdHeight - 1 |
| 1 | RASET / SETROW    | hwColStart/End | lcdWidth - 1  |

`updateWriteCache()` MV=1 のとき `casetXS = hwRowStart`、`rasetYS = hwColStart`
となるため、上記の格納先で CASET 値が論理 X、RASET 値が論理 Y に正しくマップされる。

### 変更ファイル

- `lib/src/st7789_controller.cpp`: `CMD_CASET` / `CMD_RASET` ハンドラを MV で分岐
- `lib/src/ssd1331_controller.cpp`: `CMD_SETCOLUMN` / `CMD_SETROW` ハンドラを MV で分岐

### SSD1306 への影響

SSD1306 には MV 相当ビットがなく `updateWriteCache()` も固定実装のため、
本バグの影響を受けない。

## 影響範囲

| コントローラ | 影響 |
|------------|------|
| ST7789  | MADCTL MV=1 時に描画ウィンドウ X/Y が反転し画像崩れ → **修正済み** |
| SSD1331 | SETREMAP REMAP_ADDR_INC=1 時に同様の画像崩れ → **修正済み** |
| SSD1306 | MV 相当ビットなし → **影響なし** |
