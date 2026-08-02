# 実装メモ

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
