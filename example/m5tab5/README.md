# LcdTap example for M5Stack Tab5 (ESP32-P4)

Tab5 を SPI/I2C 接続の LCD ディスプレイとして使えるようにする実装例。
背面ポート (M5-Bus / Grove) で LCD コントローラコマンドを受信し、
1280x720 の内蔵 MIPI-DSI パネルに M5GFX 経由で表示する。

## 特徴

- **SPI スレーブ**: PARLIO RX ユニットで SCK を外部クロックとして
  MOSI + D/C + CS の各レーンを毎ビットサンプリングし、ソフトウェアで
  CS フレーミングする (soft delimiter 常時受信)。
  マスタ側の CS / D/C の使い方に制約なし。
  ※ CS を level delimiter の valid 信号として使う構成は、enable 同期
  (SCK ドメイン 2 段 FF) によりフレーム先頭 2 サンプルが欠落するため
  使用しない。取り込み開始時にも同種の同期でエッジが消費されるため、
  開始時に G52 (無接続ピン) から自前のダミークロックを注入して同期を
  先に済ませる「プライミング」を行う。プライミング中は CS レーン入力を
  GPIO matrix の定数 1 に切り替えるため、注入サンプルは CS アイドルとして
  自然に破棄され、同時に再同期バリア (realign 前の滞留データの破棄境界)
  として機能する。再プライミング (キャプチャ再起動) はキャプチャ停止
  検出時 (5 秒間 DMA チャンクが届かない場合) のみ。RESX 立ち下がり時は
  キャプチャを止めずにバリアだけを注入し、リセット前に DMA パイプへ
  滞留していたデータを破棄する (SSD1306 の自動ラップに依存するマスタで
  アドレスポインタがズレるのを防ぐ)。注入は RESX が Low の間 = バスが
  静穏な間のみ行い、リセット線のバウンスでは実データを切らない。
- **I2C スレーブ**: `i2c_ll` HAL 直接制御 + ハードウェアクロックストレッチ
  (lovyan03 氏の [ESP32_I2C_slave_example](https://github.com/lovyan03/ESP32_I2C_slave_example) ベース)。
  SSD1306 プロトコル (制御バイト bit6 = D/C#, bit7 = Co) をデコード。
- **仮想キーパッド**: 物理ボタンの代わりに、タッチまたは本体の姿勢変化で
  画面下端 (ユーザ視点、IMU 追従) に十字キー + Enter を半透明表示。
  5 秒間無操作で 2 秒かけてフェードアウト。
- **OSD**: Enter キーでメニューを開く。フレームバッファの表示向きは
  OSD 設定に従う (本体姿勢とは非連動)。OSD 非表示中は左右キーで
  出力回転を切り替え。
- **設定保存**: OSD の Apply で NVS に保存。起動時に画面をタッチしたまま
  にするとデフォルト設定で起動する。

## 配線

| 信号 | GPIO | 場所 | 備考 |
|---|---|---|---|
| SPI SCK | G17 | M5-Bus | PARLIO 外部クロック入力 |
| SPI MOSI | G16 | M5-Bus | PARLIO データレーン 0 |
| SPI D/C | G18 | M5-Bus | PARLIO データレーン 1 |
| SPI CS | G45 | M5-Bus | active-low、プルアップ |
| RESX | G19 | M5-Bus | active-low、プルアップ、SPI/I2C 共用 |
| (予約) | G52 | M5-Bus | プライミング用ダミークロック。**無接続のこと** |
| (予約) | G51 | M5-Bus | プライミング用 CS 代替 (High 駆動)。**無接続のこと** |
| I2C SDA | G53 | Grove | スレーブアドレス 0x3C |
| I2C SCL | G54 | Grove | 外付けプルアップ (2.2k–10kΩ) 推奨 |

ピン割り当ては `include/app_config.h` で変更できる。

## ビルド

公式 platformio の espressif32 プラットフォームは ESP32-P4 に対応していない
ため、community の pioarduino プラットフォーム (Arduino-ESP32 3.x /
ESP-IDF v5.5 ベース) を使用する。`platformio.ini` に設定済み。

```sh
pio run              # ビルド
pio run -t upload    # 書き込み
pio device monitor   # シリアルログ (115200 bps)
```

コアライブラリ `lib/` は `lib_deps = lcdtap=symlink://../../lib` で参照
される (`lib/library.json` により PlatformIO ライブラリとして認識)。

## 制約・注意事項

- 対応バスは **I2C** と **SPI (4-wire)** のみ。SPI 3-wire と 8bit
  パラレルは非対応 (OSD で選択しても無視される)。
- 画面のリフレッシュはベストエフォート (LcdTap のフレームバッファを
  ストリップ単位で DSI パネルへ転送)。
- SPI 最大クロックは PARLIO RX の外部クロック上限に依存する。62.5 MHz
  を目標とするが、GPIO matrix 遅延の影響を含め実測での確認が必要。
- 起動時に R/G/B の縦バーが表示される (左から赤・緑・青)。色順が
  異なる場合は RGB565 バイトオーダーの問題なので `display_out.cpp` の
  pushImage 周りを確認すること。
- CS デアサートによる即時フラッシュは行わないため、マスタが送信を
  完全に停止すると、最後の DMA チャンク未満 (最大 `SOFT_EOF_BYTES` =
  2048 raw byte ≒ payload 512 byte) のデータ反映が次のクロック到来まで
  遅延することがある。連続的に描画するマスタでは実害なし。
- CS デアサート時にバイト境界に満たないビットは破棄され、`bitDrop`
  カウンタでシリアルログから観測できる。`csFrames` は CS フレーム数。
- `app_config.h` の `SPI_RAW_DUMP_ENABLE` を有効にすると、受信 raw
  サンプルの先頭 64 byte がシリアルへ hex ダンプされる (ビット配置の
  実機検証用。RESX で再アーム)。
- NVS 書き込み中は PARLIO の ISR が一時停止する (キャプチャが短時間
  止まる) が、保存は OSD 操作中にしか起きないため実用上問題ない。
