# 0007: CS Rise SM Reset Race Condition

## 現象

CS の立ち上がりエッジで PIO ステートマシン (SM) を初期化する処理を実装していたが、CS
がすぐに再び立ち下がって次のコマンドが入力された場合に、CPU からの SM 初期化が間に
合わずコマンドが抜け落ちる現象があった。

## 原因

`gpioIrqHandler` 内で CS 立ち上がりを検出し、`resetPioSm()` を呼び出して SM を停止・
FIFO クリア・再起動していた。しかし、CPU ベースの割り込みハンドラ実行には一定のレイ
テンシが伴うため、CS が短時間で再アサートされると SM 初期化の完了前に次のビットが
入力され始め、先頭データが欠落した。

## 対処

CS 立ち上がりエッジによる SM 初期化機能を削除した。

削除した処理:
- `gpioIrqHandler` 内の `GPIO_IRQ_EDGE_RISE` ブロック (`resetPioSm()` 呼び出し)
- `switchInterface` / 初期化コードの `gpio_set_irq_enabled(PIN_SPI_CS, GPIO_IRQ_EDGE_RISE, ...)` 呼び出し

RST ピンのエッジ割り込みによる SM リセット (`resetPioSm()`) は引き続き動作する。

## 対象ファイル

- `example/pico2_universal/src/main.cpp`
- `example/pico2_st7789/src/main.cpp`
- `example/pico2_ssd1306/src/main.cpp`

## 補足

CS ごとの SM リセットが本来目的としていた「部分受信バイトの破棄」については、DMA
リングバッファ方式では実用上問題が顕在化していない。SPI 通信が正常に行われる限り CS
はバイト境界で立ち上がるため、SM のビット位相がずれたまま次のトランザクションに持
ち越されるリスクは低い。
