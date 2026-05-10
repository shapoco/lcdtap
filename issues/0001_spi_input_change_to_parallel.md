# SPI 入力をパラレルにする

## 概要

SPI スレーブの受信のスループットが足りないため、SPI 入力と Raspberry Pi Pico2 の間にシフトレジスタと分周器を挿入し、SPI 入力をパラレルに変換する。

## 変更内容

SPI 入力の MOSI と SCLK を 8 ビットシフトレジスタ IC 74HC4094 に接続し、パラレル出力を Pico 2 に入力する。

SCLK はカウンタで分周した上で Pico 2 に入力する。カウンタには 74HC4040 を使用し、SCLK の立ち下がりでカウントアップする。カウンタの CLR 端子に CS を接続して CS=High でゼロクリアすることで、CS がアサートされていない間はクロックが Pico 2 に入力されないようにする。

CS=High のときはクロックが Pico 2 に入力されないので Pico 2 は CS を見る必要が無く、単に SCLK/8 の立ち下がりでパラレルデータと DCX をサンプリングすればよい。

```
CS      111100000000000000000000000000000000001111
SCLK    000000101010101010101010101010101010100000
DCX     xxxxx00000000000000001111111111111111xxxxx
MOSI    xxxxxAABBCCDDEEFFGGHHIIJJKKLLMMNNOOPPxxxxx

counter 000000011223344556677001122334455667700000
SCLK/8  000000000000011111111000000001111111100000
Q1      xxxxxxxxxxxxxxxxxxxxxHHHHHHHHHHHHHHHHPPPPP
Q2      xxxxxxxxxxxxxxxxxxxxxGGGGGGGGGGGGGGGGOOOOO
Q3      xxxxxxxxxxxxxxxxxxxxxFFFFFFFFFFFFFFFFNNNNN
Q4      xxxxxxxxxxxxxxxxxxxxxEEEEEEEEEEEEEEEEMMMMM
Q5      xxxxxxxxxxxxxxxxxxxxxDDDDDDDDDDDDDDDDLLLLL
Q6      xxxxxxxxxxxxxxxxxxxxxCCCCCCCCCCCCCCCCKKKKK
Q7      xxxxxxxxxxxxxxxxxxxxxBBBBBBBBBBBBBBBBJJJJJ
Q8      xxxxxxxxxxxxxxxxxxxxxAAAAAAAAAAAAAAAAIIIII
sampling                     ^               ^
```

SCLK は最大 62.5 MHz なので、SCLK/8 は最大 7.8125MHz となる。

## ピンアサイン変更

| GPIO | 旧 | 新 |
|------|----|----|
| 2 | SPI SCK | PAR BCLK（SCLK/8, 74HC4040 Q3） |
| 3 | SPI MOSI | PAR DCX（JMP_PIN） |
| 4 | SPI DCX | PAR D[0]（74HC4094 Q1, LSB） |
| 5 | SPI CS | PAR D[1]（74HC4094 Q2） |
| 6 | SPI RESX | PAR D[2]（74HC4094 Q3） |
| 7 | DBG_FRAME | PAR D[3]（74HC4094 Q4） |
| 8 | — | PAR D[4]（74HC4094 Q5） |
| 9 | — | PAR D[5]（74HC4094 Q6） |
| 10 | CFG_SCALE_MODE0 | PAR D[6]（74HC4094 Q7） |
| 11 | CFG_SCALE_MODE1 | PAR D[7]（74HC4094 Q8, MSB） |
| 12-19 | DVI（変更なし） | DVI（変更なし） |
| 20 | CFG_LCD_SIZE（変更なし） | 変更なし |
| 21 | CFG_DVI_RES（変更なし） | 変更なし |
| 22 | — | PAR RESX（旧 GPIO 6） |
| 25 | LED（変更なし） | 変更なし |
| 26 | — | CFG_SCALE_MODE0（旧 GPIO 10） |
| 27 | — | CFG_SCALE_MODE1（旧 GPIO 11） |
| 28 | — | DBG_FRAME（旧 GPIO 7） |

`in pins, 8` (LEFT shift, IN_BASE=GPIO 4) のビット対応：
- ISR[0] = GPIO 4 = 74HC4094 Q1 = D0（LSB、最後に入ったビット）
- ISR[7] = GPIO 11 = 74HC4094 Q8 = D7（MSB、最初に入ったビット）
- `in y,1` の後に `in pins,8` を実行 → bit[8]=DCX, bits[7:0]=バイト（旧フォーマットを維持）

## ファームウェア変更

### spi_slave.pio → par_slave.pio（全面書き換え）

旧: 62.5MHz SCLK を 1 ビットずつサンプリング（4 PIO サイクル/ビット）
新: SCLK/8 の立ち下がりを待ってパラレルサンプリング（余裕十分）

```asm
.define public PAR_BCLK_PIN  2

.program par_slave_with_dcx
    wait 0 gpio PAR_BCLK_PIN    ; スタートアップ同期

.wrap_target
    wait 1 gpio PAR_BCLK_PIN    ; SCLK/8 立ち上がり待ち（4クロック目）
    wait 0 gpio PAR_BCLK_PIN    ; SCLK/8 立ち下がり待ち（バイト確定）

    set  y, 0
    jmp  pin, dcx_high
    jmp  do_sample
dcx_high:
    set  y, 1
do_sample:
    in   y, 1        ; DCX → ISR bit[8]（8シフト後）
    in   pins, 8     ; D[7:0] をパラレルサンプリング
    push noblock
.wrap
```

ビットループが不要になりインストラクション数が大幅に削減された。

### config.h

- `PIN_SPI_*` → `PIN_PAR_*` に全面変更
- CS ピンの定数を削除
- RESX → GPIO 22、CFG_SCALE_MODE0/1 → GPIO 26/27、DBG_FRAME → GPIO 28 に移動

### main.cpp

- `spi_slave_init()` → `par_slave_init()` に置き換え（データピン 8 本をループで初期化）
- CS 関連変数（`spi_cs_sync_idx`, `spi_cs_sync_pending`）と同期ロジックを削除
- CS の GPIO IRQ 登録を削除
- DMA・リングバッファ処理・dispatch ロジックは変更なし

### CMakeLists.txt

- `pico_generate_pio_header` の対象を `par_slave.pio` に変更

## 結果

映像出力に成功。

根本原因は断定できていないが、旧実装で DCX=Low（コマンドバイト）が正しく受信
できていなかった問題（`issues/0001` 参照）は本変更で解消した。
DCX が CS アサート後しばらく変化するようなタイミング（セットアップ違反）であっても、
SCLK/8 立ち下がりでのサンプリングであれば常に安定した DCX 値を取得できるため、
タイミング余裕の改善が DCX 誤読の解消に直結したと推測される。

