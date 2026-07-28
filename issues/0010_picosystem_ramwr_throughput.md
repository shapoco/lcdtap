# PicoSystem 画像乱れ: RAMWR ピクセル処理のランレングス最適化

## 症状

PicoSystem (SPI 62.5 MHz → 外部デシリアライザ → 8bit パラレル、RGB444) で
画像の乱れが発生するようになった。

## 解析

- RGB444 は 1.5 バイト/px のため、同じ 62.5 MHz SPI でもピクセルレートは
  RGB565 より 33% 高い 5.2 Mpx/s になる。
- 従来の `writePixelRgb565()` 経由のループは、`*writePtr` への uint16_t
  ストアが `this` の uint16_t メンバ (cachedInverter / ramwrX / casetXE /
  writePtr) とエイリアス し得るため、コンパイラが毎ピクセル全メンバを
  リロード/ストアするコードを生成していた (約 21〜24 命令/px)。
  BGR / エンディアン分岐のループ外巻き上げは GCC が既に行っており、
  ボトルネックではなかった。
- リング走査等を含めた Core 0 の入力経路コストは予算 (319.2 MHz 時
  約 61 cycles/px) に対しマージン約 1.5 倍しかなく、OSD 描画・XIP
  フェッチ等のスパイクでリングバッファ (16 KB = 入力 4096 バイト分 =
  524 µs) が溢れて乱れになる。

## 対策

`processRamwrDataImpl` の RGB565_BE / RGB444_HPACK2_H2L_BE の tight loop を
ランレングス方式に置換 (`ramwrRgb565RunImpl` / `ramwrRgb444RunImpl`)。

1. **行単位の run 分割**: `run = casetXE - ramwrX + 1` を先に計算し、内側
   ループから行折り返し判定を排除。書き込み状態 (writePtr / x / inverter)
   をレジスタ常駐化。dirty tracking 時は run を 64px セグメント境界でも
   分割し、従来のセグメント粒度を維持。
2. **2px ペア 32bit ストア**: `hStep == +1` のとき 2 ピクセルを 1 つの
   32bit `str` に融合。RGB565 は先頭 1px の peel でアライン。RGB444 は
   ペア区切りがストリーム側で固定のため、非整列時はスカラーループに
   フォールバック。
3. **RGB444 pending キャリー**: 行折り返しが 3 バイトグループの中央に
   落ちる場合 (CASET 幅が奇数)、px0 で行を閉じ px1 を次 run の先頭に
   持ち越す。総ピクセル数は偶数なのでキャリーは必ず消費される。
4. **stride のテンプレート化**: 入力ストライド (1 = パック済み、
   4 = PIO リングワード、その他 = 汎用) をテンプレート引数化。
5. **SRAM 常駐化**: GCC はテンプレート実体化で section 属性を無警告で
   無視する (GCC PR 70435) ため、テンプレート本体を always_inline にし、
   RP2350 の全入力ドライバが使う stride=4 の組み合わせのみ非テンプレート
   関数 (`ramwrRgb565RunRing` 等 4 つ) に stamp して `LCDTAP_RAM_FUNC` を
   付与。SRAM コスト約 3.9 KB。

## 結果

実機 ELF の逆アセンブル実測:

| パス | 変更前 | 変更後 |
|---|---|---|
| RGB565_BE 内側ループ | 約 24 命令/px | 12 命令/2px |
| RGB444 内側ループ | 約 21〜22 命令/px | 23 命令/2px |

PicoSystem 実機で乱れの解消を確認。

## テスト

`example/pico2_common/test_host/ramwr_test.cpp` を追加。独立した参照モデル
とのフレームバッファ全画素比較を、22 シナリオ (MADCTL 8 方位・BGR・LE・
反転・奇数/非整列/1px ウィンドウ・ウィンドウ周回・奇数幅バッファ) ×
チャンク 7 種 × stride 3 種 × dirty 有無で実施。既存の dirty_test /
displaylink_span_test も全パス。

注: MV=1 でも CASET は常に論理 X 軸を駆動する (issue #0009)。テストの
参照モデルで当初これを取り違えて FAIL したが、ライブラリ側は正しい。

## 変更ファイル

- `lib/src/controller_base.hpp`
- `lib/src/lcdtap.cpp`
- `example/pico2_common/test_host/ramwr_test.cpp` (新規)

## 残件

- ESP32 (m5tab5) は RGB565 経路が新実装に切り替わる (RGB444 は ESP 専用
  ブロックを温存) ため、実機での再確認が必要。
- さらにマージンが必要な場合: spi_slave.cpp のリング走査ワードバッチ化、
  `SPI_RING_BUF_LOG2` 14→16 の拡大。
