# 0013: RGB666-LA デコードで R チャネルの LSB が G チャネルの MSB に混入する

## 現象

`InterfaceFormat::RGB666_UNPACK_LA8_BE` (ST7789/ILI9341 の COLMOD=0x06、18bpp
アンパック) のとき、ワイヤ上の R チャネル bit0 (byte0 の bit2) が 1 かつ
G チャネル MSB が 0 の画素で、フレームバッファの G 値が +32 (bit10) ずれる。
乱数画像では約 25% の画素に発生する。DVI 出力では G が 1/2 段明るくなる
subtle な色ずれとして現れる。

## 発見の経緯

example/test_pico2 (実機テスト装置) のホスト側閉ループテスト
(`test_host/verify_test.cpp`) で検出。テストパターン生成 → LcdTap 実体への
RAMWR 投入 → getframebuffer 相当のシリアライズ → ストリーミング期待値比較、
という経路で `ST7789 colmod=06` の全回転が不一致率 24.8% で NG となった。
不一致ビットが常に bit10 (G の MSB)、発生率が「R6 LSB=1 かつ G6 MSB=0 の
確率 (1/4)」と一致することから特定。

## 原因

`lib/src/lcdtap.cpp` の `processRamwrDataImpl()` RGB666_UNPACK_LA8_BE ケース:

```cpp
pixel |= (data[i] & 0xFCu) << 8;  // R5
```

`0xFC` は byte0 の bit7:2 (= R6 全 6 bit) を残すため、`<< 8` で
bits15:10 に配置され、RGB565 の R フィールド (bits15:11) から 1 bit
はみ出して G の MSB (bit10) に OR される。コメントの意図
(`R5 = byte0>>3`) どおりなら上位 5 bit のみ残すべき。

同じ式が leftover ドレイン側とタイトループ側の 2 箇所にあった。

## 修正

マスクを `0xF8` に変更し、R6 の上位 5 bit だけを bits15:11 に配置する
(2 箇所)。

```cpp
pixel |= (data[i] & 0xF8u) << 8;  // R5
```

RGB666_UNPACK_RA8_BE 側は `(byte >> 1) & 0x1F` で正しくマスクしており
影響なし。

## 検証

- `example/test_pico2/test_host/verify_test.cpp` の
  `ST7789 colmod=06` / `ILI9341 fmt=RGB666LA` 系ケースが全回転で PASS。
- 既存の `example/pico2_common/test_host/ramwr_test.cpp` は RGB666 を
  カバーしていなかったため回帰なし。
