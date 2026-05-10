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