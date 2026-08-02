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
- 設定は Flash に保存される

## セットアップ

1. `build.sh` でビルドし、`build/lcdtap_pico2w_remote.uf2` を書き込む。
2. ブラウザで [LcdTap Remote Setup](https://shapoco.github.io/lcdtap/remote/)
   を開き、WebSerial で接続して SSID / パスフレーズ等を書き込む。
3. Pico2W が再起動して WiFi 接続が試行される。接続に成功すると
   LED が常時点灯になり、`http://lcdtap.local/` (または IP アドレス) で Web UI が開ける。

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
