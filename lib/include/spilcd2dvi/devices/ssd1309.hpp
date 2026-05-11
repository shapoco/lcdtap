#pragma once

#include <cstdint>

// SSD1309 コマンド定数
// 注: パラメータを持つコマンドはすべて DCX=0 バイトとして送られる。
//     DCX=1 バイトは常に GDDRAM データ (MONO_VPACK)。

namespace sl2d {
namespace ssd1309 {

// 下位列アドレス指定 (ページアドレッシングモード用, 0x00-0x0F)
static constexpr uint8_t CMD_SET_LOWER_COL_MASK = 0x0F;
static constexpr uint8_t CMD_SET_LOWER_COL_BASE = 0x00;

// 上位列アドレス指定 (ページアドレッシングモード用, 0x10-0x1F)
static constexpr uint8_t CMD_SET_HIGHER_COL_MASK = 0x0F;
static constexpr uint8_t CMD_SET_HIGHER_COL_BASE = 0x10;

// メモリアドレッシングモード設定 (パラメータ1バイト)
// パラメータ: 0x00=水平, 0x01=垂直, 0x02=ページ (デフォルト)
static constexpr uint8_t CMD_SET_ADDR_MODE = 0x20;

// 列アドレス設定 (パラメータ2バイト: start, end) — 水平/垂直モード用
static constexpr uint8_t CMD_SET_COL_ADDR = 0x21;

// ページアドレス設定 (パラメータ2バイト: start, end) — 水平/垂直モード用
static constexpr uint8_t CMD_SET_PAGE_ADDR = 0x22;

// 表示開始ライン設定 (0x40-0x7F, 下位6ビットが開始行)
static constexpr uint8_t CMD_SET_START_LINE_MASK = 0x3F;
static constexpr uint8_t CMD_SET_START_LINE_BASE = 0x40;

// コントラスト設定 (パラメータ1バイト: 0x00-0xFF)
static constexpr uint8_t CMD_SET_CONTRAST = 0x81;

// セグメントリマップ: col0 → SEG0
static constexpr uint8_t CMD_SEG_REMAP_0 = 0xA0;

// セグメントリマップ: col127 → SEG0 (水平反転)
static constexpr uint8_t CMD_SEG_REMAP_1 = 0xA1;

// 通常表示 (白ピクセル=GDDRAM bit 1)
static constexpr uint8_t CMD_NORMAL_DISPLAY = 0xA6;

// 反転表示 (白ピクセル=GDDRAM bit 0)
static constexpr uint8_t CMD_INVERT_DISPLAY = 0xA7;

// マルチプレックス比設定 (パラメータ1バイト: 無視)
static constexpr uint8_t CMD_SET_MULTIPLEX = 0xA8;

// 表示オフ
static constexpr uint8_t CMD_DISPLAY_OFF = 0xAE;

// 表示オン
static constexpr uint8_t CMD_DISPLAY_ON = 0xAF;

// ページ開始アドレス設定 (ページアドレッシングモード用, 0xB0-0xB7)
static constexpr uint8_t CMD_SET_PAGE_START_MASK = 0x07;
static constexpr uint8_t CMD_SET_PAGE_START_BASE = 0xB0;

// COM スキャン方向: COM0→COM63 (正順)
static constexpr uint8_t CMD_COM_SCAN_INC = 0xC0;

// COM スキャン方向: COM63→COM0 (垂直反転)
static constexpr uint8_t CMD_COM_SCAN_DEC = 0xC8;

// 表示オフセット設定 (パラメータ1バイト: 無視)
static constexpr uint8_t CMD_SET_DISPLAY_OFFSET = 0xD3;

// クロック分周比/発振周波数設定 (パラメータ1バイト: 無視)
static constexpr uint8_t CMD_SET_CLK_DIV = 0xD5;

// プリチャージ期間設定 (パラメータ1バイト: 無視)
static constexpr uint8_t CMD_SET_PRECHARGE = 0xD9;

// COM ピンハードウェア構成 (パラメータ1バイト: 無視)
static constexpr uint8_t CMD_SET_COM_PINS = 0xDA;

// Vcomh 非選択レベル設定 (パラメータ1バイト: 無視)
static constexpr uint8_t CMD_SET_VCOMH = 0xDB;

// NOP
static constexpr uint8_t CMD_NOP = 0xE3;

}  // namespace ssd1309
}  // namespace sl2d
