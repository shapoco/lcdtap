#pragma once

#include "controller_base.hpp"

namespace lcdtap {

// SSD1309 コントローラ実装
//
// ST7789 との主な差異:
// - DCX=0 バイトがコマンド本体とパラメータ両方を担う
// - DCX=1 バイトは常に GDDRAM データ (MONO_VPACK: 1バイト=縦8ピクセル)
// - RAMWR 相当の明示コマンドはなく isRamWriteCommand() は常に true
// - アドレッシングはページベース (水平/垂直/ページの3モード)
class Ssd1309Controller : public ControllerBase {
 public:
  uint8_t ssdAddrMode;     // 0=水平, 1=垂直, 2=ページ (デフォルト)
  bool ssdSegmentRemap;    // true=A1 (col127→SEG0, 水平反転)
  bool ssdComFlip;         // true=C8 (COM63→COM0, 垂直反転)
  uint8_t expectedParams;  // 現コマンドの残りパラメータバイト数
  uint8_t pageColLow;      // ページモード列アドレス 下位ニブル
  uint8_t pageColHigh;     // ページモード列アドレス 上位ニブル

  uint16_t logicalWidth() const override;
  uint16_t logicalHeight() const override;
  void updateWriteCache() override;
  void softReset() override;
  void dispatchCommand(uint8_t cmd) override;
  void feedDataByte(uint8_t byte) override;
  bool isRamWriteCommand() const override;
  void processRamwrData(const uint8_t* data, uint32_t numBytes, uint32_t stride) override;

 private:
  void applyPageModeCol();
};

}  // namespace lcdtap
