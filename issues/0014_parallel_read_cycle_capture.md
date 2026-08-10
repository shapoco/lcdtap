# 0014: パラレル I/F でホストのリードサイクルがコマンドとして混入する

## 現象

pico2w_remote + パラレル I/F (SC1602/SC2004, 6800 バス, R/W→CS・E→WR 接続) で、
実機 LCD は正常に表示されるのに LcdTap 側では文字抜け・化け・CGRAM グリフの崩れが
発生した。Statistics では RX Drop / Overflow は 0 のまま Unknown Commands が徐々に
増加 (Last Unknown Command=0x00)。WiFi を無効化すると大幅に改善するが完全には
消えない。pico2_universal では発生しない。オシロで CS/WR/DC の波形に乱れはない。

Command Dump を pico2_universal と比較すると、DC=0 の余計なバイトが散発的に挿入
されており、値は 0x10→0x12→0x13→… や 0x43→0x47→…→0x50 のような単調増加、
または同一値の繰り返しだった。

## 原因

挿入バイトの正体は、ホストがコマンド/データ書き込みごとに行う **ビジーフラグ
ポーリング (RW=1, RS=0 のリードサイクル) の応答 (BF | アドレスカウンタ)** だった。
AC は書き込みのたびに増えるため単調増加になり、DDRAM 1 行目 (0x00–0x13)・
2 行目 (0x40–0x53) のアドレス範囲と観測値が一致する。

R/W→CS 接続でのリード除去は「CS (=R/W) 立ち上がりの GPIO IRQ で
`spiSlaveResetSm()` を呼び、SM をプログラム先頭の `wait 0 gpio PAR_CS_PIN` に
戻す」ことで実現されていたが、これは割り込みレイテンシ頼みの競合だった。ホストは
R/W を上げてから約 2 µs 後にリードの E 立ち下がり (= サンプル点) を発生させるため、
IRQ→SM リセットが 2 µs を超えるとリード応答がキャプチャされる。

pico2w_remote では Core 0 が cyw43/lwIP/USB CDC のフラッシュ常駐コードを高頻度で
実行するため、XIP キャッシュミスとコア間フラッシュアービトレーションで Core 1 の
IRQ 経路 (SDK の GPIO ディスパッチと `spiSlaveResetSm` 自体がフラッシュ上) の
レイテンシが µs オーダーに伸びるスパイクが発生し、競合に負けてリードバイトが
混入した。WiFi ON で頻発・OFF で残留・pico2_universal では発生しない、という
観測はすべてこの XIP 競合の度合いで説明できる。

混入した BF|AC バイトはコマンドとして実行され、各症状を引き起こす:

- 0x40–0x7F → Set CGRAM Address → 後続データが CGRAM を破壊 (グリフ崩れ)
- 0x80– (BF=1 時) → Set DDRAM Address → カーソル移動 (文字抜け・位置ずれ)
- 0x10–0x1F → Cursor/Display Shift → 表示ずれ
- 0x00 (AC=0 時) → Unknown Command 0x00

## 対処

リード除去を IRQ から PIO プログラム内のレベル判定に移し、競合を原理的に排除した。

- `parallel_8bit.pio`: サンプリング (`in pins, 9`) の直後に `jmp pin, drop` を追加。
  EXECCTRL_JMP_PIN = CS (8080: CS#, 6800: R/W) が High なら `mov isr, null` で
  破棄する。判定はサンプルの 1 サイクル後で、ホストは E 立ち下がり後も約 1 µs は
  R/W を保持するため競合しない。トランザクション単位の同期だった先頭の
  `wait 0 gpio PAR_CS_PIN` はバイト単位のゲートで置き換えられたため削除。
- `bus_input.cpp` `parSlaveInit()`: `sm_config_set_jmp_pin(&c, cfg.pinCs)` を追加。
- `bus_input.cpp` `busInputSwitch()`: PARALLEL の CS 立ち上がり IRQ 有効化を削除
  (SPI_3LINE / SPI_4LINE は従来どおり)。
- `spi_slave.cpp` `spiSlaveResetSm()`: `__not_in_flash_func` を付与し、SPI モードで
  残る CS-rise IRQ 経路のレイテンシも XIP 競合の影響を受けにくくした。

8080 ホストに対しても「CS=High のストローブは自分宛でないので捨てる」という意味に
なり、同じロジックで両バススタイルに正しく働く。

## 対象ファイル

- `example/pico2_common/src/parallel_8bit.pio`
- `example/pico2_common/src/bus_input.cpp`
- `example/pico2_common/src/spi_slave.cpp`

## 補足

- CS 立ち上がり IRQ による SM リセットの競合問題は issue 0007 と同根 (割り込み
  レイテンシ頼みの制御は入力ストリームに対して原理的に競合する)。
- ダンプで稀に見られた「抜け」は、IRQ リセットが遅延して次のライトストローブ付近で
  FIFO クリアが走ったケースと考えられ、IRQ 削除により同時に解消される見込み。
- 6800 モードで検証したホスト: `sc1602_test` (RP2040, 全書き込み前に BF ポーリング、
  DDRAM リードバックテストあり)。
