# シフトレジスタ IC を 74HC595 に変更

コミット: 2f4935bd801c69ca9c66d9e0da3b0855d7c7fe58

## 概要

SPI クロックの高速化（最大 62.5 MHz）に対応するため、シリアル→パラレル変換に使用する
シフトレジスタ IC を 74HC4094 から 74HC595 に変更した。

## 変更理由

74HC4094 は CP（クロック）入力の立ち上がりでシリアルデータをシフトし、ST（ストローブ）
入力の立ち上がりで Q 出力を更新する構成だった。高速クロックでの動作において出力の
セットアップ・ホールドタイミングに余裕がなく、動作の信頼性に問題があった。

74HC595 は SRCLK（シフトレジスタクロック）の立ち上がりで取り込み、RCLK（ラッチクロック）
の立ち上がりで Q 出力を更新するため、74HC4040 との組み合わせで安定した動作が得られる。

## 回路変更

### シフトレジスタ

| 旧 | 新 |
|----|----|
| 74HC4094 | 74HC595 |
| CP ← SPI SCLK | SRCLK ← SPI SCLK |
| DS ← SPI MOSI | SER ← SPI MOSI |
| ST ← （未接続 or 固定 HIGH） | RCLK ← 74AHC1G04 出力（BCLK） |

### BCLK ラインへのインバータ追加

74HC4040 Q3 出力（SCLK/8）は 74AHC1G04 シングルインバータを通して BCLK を生成するよう変更した。

```
SPI SCLK ──→ 74HC4040 CP
SPI CS   ──→ 74HC4040 CLR（active-high）
74HC4040 Q3 ──→ 74AHC1G04 IN ──→ BCLK ──┬──→ Pico 2 GPIO 2
                                         └──→ 74HC595 RCLK
```

変更前は 74HC4040 Q3 が直接 Pico 2 に入力されていた。
変更後は 74AHC1G04 で反転した後の信号が BCLK として Pico 2 および 74HC595 RCLK に入力される。

### BCLK 極性

| | BCLK レベル |
|-|-------------|
| CS 非アサート（アイドル） | HIGH（74HC4040 Q3=0 → 反転 → HIGH） |
| バイト転送中 | LOW |
| バイト完了時（8 クロック後） | HIGH（74HC4040 Q3 立ち下がり → 反転 → 立ち上がり）|

74HC595 は BCLK 立ち上がり（= RCLK 立ち上がり）で Q 出力を確定する。

## ファームウェア変更（par_slave.pio）

BCLK の極性反転に合わせて、スタートアップ同期とメインループの wait 命令のロジックを変更した。

### 変更前（74HC4094 構成）

```asm
    wait 0 gpio PAR_BCLK_PIN    ; スタートアップ同期：BCLK が LOW になるまで待つ
.wrap_target
    wait 1 gpio PAR_BCLK_PIN    ; BCLK 立ち上がり待ち（バイト転送中）
    wait 0 gpio PAR_BCLK_PIN    ; BCLK 立ち下がり待ち（バイト完了）
```

### 変更後（74HC595 + 74AHC1G04 構成）

```asm
    wait 1 gpio PAR_BCLK_PIN    ; スタートアップ同期：BCLK が HIGH になるまで待つ
.wrap_target
    wait 0 gpio PAR_BCLK_PIN    ; BCLK LOW 待ち（バイト転送中）
    wait 1 gpio PAR_BCLK_PIN    ; BCLK HIGH 待ち（バイト完了、74HC595 出力確定）
```

## 影響範囲

- `example/pico2_st7789/src/par_slave.pio`
- `example/pico2_ssd1309/src/par_slave.pio`
- `example/pico2_st7789/include/config.h`（コメント）
- `example/pico2_ssd1309/include/config.h`（コメント）
- `example/pico2_st7789/README.md`
- `example/pico2_ssd1309/README.md`
