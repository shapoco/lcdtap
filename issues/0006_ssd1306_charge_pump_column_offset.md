# SSD1306 初期化後の書き込み開始位置がずれる (CMD_CHARGE_PUMP 未処理)

## 現象

`pico2_universal` で SSD1306 を使用すると、フレームバッファの書き込み開始位置が column 0 にならず、画像の左上隅がフレームバッファの左上隅と一致しない。

- 一番上の page の中央あたり (column 64) から書き込みが始まる。
- 右下隅でラップアラウンドし、最後の約 1/2 page 分が左上隅から現れる。
- SSD1306 実機では正常に表示されているため、マスタ側の初期化コマンドは正しい。

## 解析

`CMD_CHARGE_PUMP (0x8D)` がコマンドヘッダにも `dispatchCommand` にも定義されておらず、`default: break;` に落ちるため `expectedParams = 0` のままになる。

続くパラメータバイト `0x14` (Charge Pump Enable) が新たなコマンドとして処理される。`0x14` は `CMD_SET_HIGHER_COL_BASE (0x10)–0x1F` の範囲に一致するため、`applyPageModeCol()` が呼ばれて意図せず `pageColHigh = 0x40, ramwrX = 64` に設定される。

```
0x8D → default (expectedParams=0 のまま)
0x14 → CMD_SET_HIGHER_COL: pageColHigh = (0x14 & 0x0F) << 4 = 0x40
         applyPageModeCol() → ramwrX = 0x40 | pageColLow = 64
```

その後の初期化シーケンスで `CMD_SET_COL_ADDR (0x21, 0x00, 0x7F)` が送られる場合は `ramwrX` が 0 にリセットされるが、送られない場合や `0x8D, 0x14` より前に送られる場合はオフセットが残ったままデータ送信が始まる。

## 対応

`CMD_CHARGE_PUMP = 0x8D` を `lib/include/lcdtap/devices/ssd1306.hpp` に追加し、`dispatchCommand` の switch 文に 1 パラメータコマンドとして登録した。

```cpp
// lib/include/lcdtap/devices/ssd1306.hpp
static constexpr uint8_t CMD_CHARGE_PUMP = 0x8Du;

// lib/src/ssd1306_controller.cpp
case CMD_CHARGE_PUMP: expectedParams = 1; break;
```
