# LcdTap pico2w_remote

Raspberry Pi Pico 2 W 向けの LcdTap 実装例。pico2_universal と同じ入力
インターフェース (I2C / 4線SPI / 3線SPI / 8bitパラレル、同一ピン配置) で
LCD コントローラのトラフィックをキャプチャし、**映像出力なし** で WiFi
経由の HTTP JSON API とデバイス内蔵 Web UI から画面を読み取ります。

## 機能

- HTTP サーバ (ポート 80)
  - `GET /` — LcdTap Monitor と同一 UI の Web ページ (ファーム内蔵)
  - `POST /api` — pico2_universal の UART I/F と同一の JSON コマンド
- USB CDC シリアル — 同じ JSON コマンド一式 + WiFi セットアップ
  (`getnetconfig` / `setnetconfig` / `netstatus`)
- mDNS — `http://<hostname>.local/` (デフォルト `lcdtap.local`)
- 設定は Flash に保存 (LcdTap 設定 = 最終セクタ、ネットワーク設定 =
  最終-1 セクタ)

## セットアップ

1. `build.sh` でビルドし、`build/lcdtap_pico2w_remote.uf2` を書き込む。
2. WiFi 未設定の間、LED が 0.25 秒点灯 / 0.75 秒消灯で点滅する。
3. ブラウザで [LcdTap Remote Setup](https://shapoco.github.io/lcdtap/remote/)
   を開き、WebSerial で接続して SSID / パスフレーズ等を書き込む
   (シリアルターミナルから `setnetconfig` を直接送ってもよい)。
4. デバイスが再起動して接続を試みる (0.25/0.25 点滅)。接続に成功すると
   LED が常時点灯になり、`http://lcdtap.local/` (または netstatus が返す
   IP) で Web UI が開ける。接続失敗時は 0.5/0.5 点滅でリトライする
   (5 秒間隔、5 回連続失敗後は 30 秒間隔)。

## LED

| 状態 | パターン |
|---|---|
| WiFi 未設定 | 0.25 s 点灯 / 0.75 s 消灯 |
| 接続試行中 | 0.25 s 点灯 / 0.25 s 消灯 |
| 接続失敗 (リトライ待ち) | 0.5 s 点灯 / 0.5 s 消灯 |
| 接続中 | 常時点灯 + アクセス時に 50 ms 消灯パルス |

## ピン配置

入力ピンは pico2_universal と同一 (GPIO0–11)。GPIO23/24/25/29 は Pico 2 W
の CYW43 (WiFi/LED) が使用するため空けてある。映像出力・キー入力は無し。

| 信号 | GPIO |
|---|---|
| RST | 0 |
| CS (SPI/パラレル) | 1 |
| SCLK / WR# | 2 |
| MOSI / D[0] | 3 |
| DC (4線SPI) / D[1] | 4 |
| D[2..7] | 5–10 |
| DC (パラレル) | 11 |
| I2C SDA / SCL | 8 / 9 |

## 実装メモ

- コア分担は pico2_universal と逆: **Core 1** が入力ドレイン
  (バス IRQ も Core 1 の NVIC に登録)、**Core 0** が cyw43/lwIP ポーリング
  + HTTP + USB CDC + LED。62.5 MHz SPI 入力が lwIP の処理バーストに
  影響されないための構成。
- Flash 書き込みとバス切替は Core 0 → Core 1 のメールボックスで依頼する
  (書き込み中 Core 1 は SRAM 上で待機)。
- clk_sys は実績のある 312 MHz (`sysClockInit312`)。CYW43 の PIO SPI は
  `CYW43_PIO_CLOCK_DIV_INT=4` で 39 MHz に抑えている。
- Web UI は `docs/monitor/` の共有ファイル (device.html / monitor.css /
  monitor.js / http_conn.js) をビルド時に gzip 圧縮して取り込む
  (`tools/embed_assets.py`)。GitHub Pages 版 Monitor と単一ソース。
- HTTP API は同時 1 リクエスト (2 本目は 503)。認証・暗号化は無いので
  信頼できる LAN 内での使用を前提とする。パスフレーズは Flash に平文で
  保存され、`getnetconfig` はパスフレーズを返さない。
- 既知の制限: CDC と HTTP の両方から `writeProtected:true` の
  getframebuffer を同時に実行すると、先に完了した側が書き込み保護を
  解除する (もう一方のキャプチャにティアリングが出る可能性)。
