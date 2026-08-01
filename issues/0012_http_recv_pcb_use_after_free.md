# 0012: pico2w_remote の HTTP 全応答が ERR_CONNECTION_RESET になる

- ステータス: 解決済み (3eacab0、実機確認済み: ページ表示・設定変更・キャプチャ取得 OK)
- 対象: example/pico2w_remote/src/http_server.cpp

## 症状

- WiFi 接続・ping 応答・USB CDC は正常。
- ブラウザで `http://lcdtap.local/` を開くと Pico 2 W の LED が 1 回
  点滅した後、Chrome が `ERR_CONNECTION_RESET` を表示しページが出ない。
- クラッシュはしていない (CDC 経由のキャプチャは継続して動作)。

## 原因

`onRecv()` (lwIP tcp_recv コールバック) の処理順の誤り:

```
if (c->state == RECV_REQ) {
  pbuf_copy_partial(...);
  tryDispatch(c);        // 小さい応答はここで送信完了し tcp_close() まで進む
}
tcp_recved(pcb, p->tot_len);   // ← 解放済み pcb への use-after-free
```

埋め込みアセットは gzip 済みで小さく、応答全体が TCP 送信バッファ
(8*MSS) に収まるため、`tryDispatch()` → `connPump()` → `tcp_close()` が
受信コールバック内で同期的に完走する。その直後に閉じた pcb へ
`tcp_recved()` を呼ぶため動作は未定義で、実機では RST 送出として現れた。
LED の 1 回点滅は accept 時の `ledActivityPulse()` で、症状と符合する。

## 修正

受信ウィンドウの解放 (`tcp_recved`) と `pbuf_free` を **dispatch より前**
に移動し、`tryDispatch()` 以降は pcb を触らない構造にした。他の
コールバック (onSent / onPoll) は connClose 後に pcb を参照しないことを
確認済み。

あわせて、POST /api ビジー時の 503 応答が 2 秒後の poll まで送信されない
問題も修正 (`startApiRequest()` 直後に `connPump()`)。

## 教訓

lwIP raw API のコールバック内で `tcp_close()` に到達しうる処理を呼ぶ
場合、その後に pcb を参照する処理 (`tcp_recved` 等) を残してはならない。
「小さい応答は recv コールバック内で完結する」ことを前提に順序を組む。
