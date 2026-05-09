# spilcd2dvi

SPI LCD コマンドを受信し、DVI-D 信号として出力するプロジェクトです。

## ディレクトリ構成

```
spilcd2dvi/
├── lib/                        # コアライブラリ（MCU非依存）
│   ├── src/                    # ライブラリ実装
│   ├── include/
│   │   └── spilcd2dvi/
│   │       └── spilcd2dvi.hpp  # 公開ヘッダ
│   └── CMakeLists.txt
├── example/                    # MCU別の実装例
│   └── pico2/                  # Raspberry Pi Pico2向け
│       ├── src/
│       │   └── main.cpp
│       ├── include/
│       └── CMakeLists.txt
├── LICENSE
├── .gitignore
└── README.md
```

## ライブラリの使い方

```cpp
#include <spilcd2dvi/spilcd2dvi.hpp>
```

## ビルド方法

### Raspberry Pi Pico2

```bash
cd example/pico2
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
make -j4
```

ビルド後、`spilcd2dvi_pico2.uf2` が生成されます。BOOTSEL ボタンを押しながら USB 接続し、ドラッグ＆ドロップでフラッシュしてください。

## ライセンス

MIT License — 詳細は [LICENSE](LICENSE) を参照してください。
