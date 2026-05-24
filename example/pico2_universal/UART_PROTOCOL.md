# UART Protocol

- ホストから「コマンド」を投げ、それに対して LcdTap が「レスポンス」を返す形で通信する。
- コマンドもレスポンスも行頭から始まり、コンテンツは JSON 文字列で構成され、改行(CRLF)で終わる。
- コマンドは次の形式。実際には改行やインデントは含まれない。
    
    ```json
    {
        "command": コマンド名(文字列),
        "params": {
            "パラメータ名1": パラメータ値1,
            "パラメータ名2": パラメータ値2,
            "パラメータ名3": パラメータ値3,
            ...
        }
    }
    ```
    
    - LcdTap 側の字句解析の簡単のため、コマンドについては各トークンは ASCII 文字のみで構成され、最長 64 文字とする。
    
- レスポンスはコマンドに応じた JSON 文字列。

## コマンドリファレンス

### hello

ホストから見てシリアルポートの先に LcdTap が繋がっていることを確認する

- コマンド:

    ```json
    {"command": "hello"}
    ```

- レスポンス:
    
    ```json
    {"response": "welcome lcdtap"}
    ```

### getparams

LcdTap が持つ設定項目の一覧を JSON で取得する

- コマンド:
    
    ```json
    {"command": "getparams"}
    ```

- レスポンス:

    ```json
    {"params":[パラメータリスト]}
    ```

パラメータリストの要素のタイプ:

- 整数:

    ```json
    {
        "id":メニューID(文字列),
        "type":"INTEGER",
        "name":アイテムラベル(文字列),
        "unit":単位(文字列) または null,
        "min":最小値(整数),
        "max":最大値(整数),
        "step":数値の変化量(整数),
        "value":現在の値(整数)
    }
    ```

- 真偽値:

    ```json
    {
        "id":メニューID(文字列),
        "type":"BOOLEAN",
        "name":アイテムラベル(文字列),
        "value":現在の値(真偽値)
    }
    ```

- 列挙値:

    ```json
    {
        "id":メニューID(文字列),
        "type":"ENUM",
        "name":アイテムラベル(文字列),
        "unit":単位(文字列) または null,
        "options":{
            "選択肢1": 値1(整数),
            "選択肢2": 値2(整数),
            "選択肢3": 値3(整数),
            ...
        },
        "value":現在の値(整数)
    }
    ```

### setparams

ホストから LcdTap の設定を一括設定する

- コマンド:

    ```json
    {
        "command": "setparams",
        "params": {
            "メニューID1": 値1(整数/真偽値),
            "メニューID2": 値2(整数/真偽値),
            ...
        }
    }
    ```

- レスポンス:

    ```json
    {"response": "ok"}
    ```

### getframebuffer

フレームバッファの内容を取得する

- コマンド:

    ```json
    {"command": "getframebuffer"}
    ```

- レスポンス:

    ```json
    {
        "width": フレームバッファの幅(整数),
        "height": フレームバッファの高さ(整数),
        "format": "RGB565",
        "data": RGB565画像をリトルエンディアンでBase64エンコードしたもの(文字列)
    }
    ```

    - スケーリングはしないが、回転、明暗の反転、R/Bの入れ替えは DVI 出力と同じ見た目になるようにする。

### dump_start

コマンドダンプのキャプチャを開始する

- コマンド:

    ```json
    {"command": "cmddump_start"}
    ```

- レスポンス:

    ```json
    {"response": "ok"}
    ```

### dump_abort

コマンドダンプのキャプチャを中止する

- コマンド:

    ```json
    {"command": "cmddump_abort"}
    ```

- レスポンス:

    ```json
    {"response": "ok"}
    ```

### dump_forcetrigger

コマンドダンプのキャプチャを強制的に開始する

- コマンド:

    ```json
    {"command": "cmddump_forcetrigger"}
    ```

- レスポンス:

    ```json
    {"response": "ok"}
    ```

### dump_getstatus

コマンドダンプの状態を取得する

- コマンド:

    ```json
    {"command": "cmddump_getstatus"}
    ```

- レスポンス:

    ```json
    {"status": "WAIT"、"ACTIVE"、"COMPLETE" のいずれか, "bytes": バッファの格納バイト数(整数)}
    ```

### dump_read

コマンドダンプの内容を取得する

- コマンド:

    ```json
    {"command": "cmddump_read"}
    ```

- レスポンス:

    ```json
    {
        "length": キャプチャされたデータの長さ(整数),
        "data": キャプチャされたコマンドデータをBase64エンコードしたもの(文字列)
    }
    ```
