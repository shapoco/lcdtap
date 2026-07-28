# 0011: SPI RX Drop — リング一周境界での受信量再構成レース

## 症状

- SPI I/F で放置中に不定期な 1〜2 バイトの RX Drop が発生する。
    - Arduboy (75 kB/s): 数分に 1 回
    - M5Stack CoreS3 (5 MB/s): 数秒に 1 回
    - 発生頻度は RX Rate にほぼ比例
- Drop 発生の瞬間、RX Peak Backlog が**ちょうど 4096** (= リングワード数) になる。
  通常時の backlog は Arduboy で 12、CoreS3 で 260-270 程度。
- RX HW Overflow = 0、TX Underrun = 0。物理的な取りこぼしはない。
- I2C (TinyJoyPad, 15 kB/s) では発生しない。

## 原因

`spiSlaveProcess()` のオーバーラン検出は受信総量を

```
totalReceived = (wrapCount + pend) * ringWords + writeIdxNow
```

と再構成していた。wrapCount (DMA_IRQ_1 ハンドラが加算)、pend (INTS1 生フラグ)、
writeIdxNow (DMA `write_addr`) は更新タイミングが独立したハードウェア状態であり、
DMA がリングを一周した瞬間に「`write_addr` は base に巻き戻り済みだが、INTS1 の
セットがまだ CPU から見えない」数十 ns の窓がある。安定化 do-while は
wrapCount / pend の変化しか監視していないため、この窓でサンプリングすると
totalReceived が**ちょうど ringWords (4096) 過小**になる。

その後の連鎖:

1. backlog が負になり、「wrap IRQ 飢餓」向けのサイレント再同期が
   `totalConsumedWords = totalReceived` と消費カウンタを 4096 小さく破壊する。
2. 数 µs 後の次の呼び出しでは wrapCount/INTS1 が正しく見えるため
   backlog = 実滞留 + 4096 になる。
    - 実滞留 0: backlog == 4096 ちょうど → ドロップ判定 (`> ringWords`) は偽。
      **Peak Backlog に 4096 が記録される**だけ。
    - 実滞留 ≥ 1: 「追い越し」誤検出 → `readIdx = writeIdxNow` で
      実際に滞留していた 1〜2 バイトを読み飛ばす。**これが観測された Drop**。

つまり本物のリングバッファ追い越しは発生しておらず、誤検出の回復処理が
自らデータを捨てていた。レースはリング一周 (wrap) のたびに
「ポーリング頻度 × 競合窓幅」の確率で起こるため、発生頻度がレートに比例する。
I2C スレーブは IRQ ハンドラがソフトウェアで writeIdx を進める方式で
wrap 再構成を使っていないため影響しない。

## 修正

受信総量の主導出を write pointer の差分累積に変更した:

- 毎回 `totalReceivedWords += (writeIdxNow - lastWriteIdx) & (ringWords - 1)`。
  この値は**過大になり得ず**、呼び出し間にリングを丸一周以上された場合
  (= 検出したい本物のオーバーラン) だけ過小になる。
- wrap カウント方式の値 (`wrapBased`) は丸一周検出専用に残し、
  `wrapBased` が先行しているときだけ採用する
  (`(int32_t)(wrapBased - totalReceivedWords) > 0` → 上書き)。
  同一コア (main ループと DMA_IRQ_1 は共に Core 0) では wrapBased が
  過大側にずれる経路はないため、この片側採用は安全。
- backlog は構造的に非負になるため、消費カウンタを書き換える
  サイレント再同期を削除 (負値クランプのみ防御的に残置)。
- `spiSlaveInitDma()` で IRQ 有効化前に残留ペンディング INTS1 をクリア。

wrap IRQ が抑止されるケース (flash 書き込み中など) で複数 wrap が 1 回に
まとめられた場合、dropWords が下限値になる点は従来と同じ。

## 検証

- CoreS3 / Arduboy を接続して放置し、RX Drop = 0・Peak Backlog が通常値の
  ままであることを確認する。
- 回帰: SPI↔I2C 切替、CoreS3 ストリーミング中の設定保存 (flash 書き込み)。
