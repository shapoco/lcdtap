#include "ssd1309_controller.hpp"

#include <lcdtap/devices/ssd1309.hpp>

namespace lcdtap {

uint16_t Ssd1309Controller::logicalWidth() const { return config.lcdWidth; }

uint16_t Ssd1309Controller::logicalHeight() const { return config.lcdHeight; }

uint32_t Ssd1309Controller::physIndex(uint32_t lcol, uint32_t lrow) const {
  return lrow * config.lcdWidth + lcol;
}

void Ssd1309Controller::updateWriteCache() {
  // MONO_VPACK モードでは writePtr キャッシュは使用しない
}

void Ssd1309Controller::softReset() {
  ssdAddrMode = 2;  // ページアドレッシング (SSD1309 デフォルト)
  ssdSegmentRemap = false;
  ssdComFlip = false;
  expectedParams = 0;
  pageColLow = 0;
  pageColHigh = 0;
  resetCommon();
  sleeping = false;  // SSD1309 にスリープモードはない
  pixelFormat = PixelFormat::MONO_VPACK;
  casetXS = 0;
  casetXE = static_cast<uint16_t>(config.lcdWidth - 1);
  rasetYS = 0;
  rasetYE = static_cast<uint16_t>(config.lcdHeight - 1);
  ramwrX = 0;
  ramwrY = 0;
}

// DCX=0 バイトをすべて受け付ける (コマンド本体とパラメータ両方)
void Ssd1309Controller::dispatchCommand(uint8_t cmd) {
  using namespace ssd1309;

  // 現コマンドのパラメータ待ち中
  if (expectedParams > 0) {
    --expectedParams;
    switch (currentCmd) {
      case CMD_SET_ADDR_MODE:
        ssdAddrMode = cmd & 0x03u;
        log("SSD1309: SET_ADDR_MODE");
        break;
      case CMD_SET_COL_ADDR:
        if (cmdDataLen == 0) {
          casetXS = cmd;
          ramwrX = casetXS;
        } else {
          casetXE = cmd;
          log("SSD1309: SET_COL_ADDR");
        }
        break;
      case CMD_SET_PAGE_ADDR:
        if (cmdDataLen == 0) {
          rasetYS = static_cast<uint16_t>(cmd * 8u);
          ramwrY = rasetYS;
        } else {
          rasetYE = static_cast<uint16_t>(cmd * 8u + 7u);
          if (rasetYE >= config.lcdHeight)
            rasetYE = static_cast<uint16_t>(config.lcdHeight - 1);
          log("SSD1309: SET_PAGE_ADDR");
        }
        break;
      default: break;  // 無視するコマンドのパラメータ
    }
    ++cmdDataLen;
    return;
  }

  // 新コマンド
  currentCmd = cmd;
  cmdDataLen = 0;
  ramwrBufLen = 0;
  expectedParams = 0;

  if (cmd >= (CMD_SET_START_LINE_BASE) &&
      cmd <= (CMD_SET_START_LINE_BASE | CMD_SET_START_LINE_MASK)) {
    // 表示開始ライン (スクロール): 受け付けるが反映しない
    return;
  }

  if (cmd >= CMD_SET_PAGE_START_BASE &&
      cmd <= (CMD_SET_PAGE_START_BASE | CMD_SET_PAGE_START_MASK)) {
    // ページアドレッシングモード: ページ設定
    uint8_t page = cmd & CMD_SET_PAGE_START_MASK;
    ramwrY = static_cast<uint16_t>(page * 8u);
    ramwrX = casetXS;
    return;
  }

  if (cmd >= CMD_SET_LOWER_COL_BASE &&
      cmd <= (CMD_SET_LOWER_COL_BASE | CMD_SET_LOWER_COL_MASK)) {
    // ページアドレッシングモード: 列アドレス下位ニブル
    pageColLow = cmd & CMD_SET_LOWER_COL_MASK;
    applyPageModeCol();
    return;
  }

  if (cmd >= CMD_SET_HIGHER_COL_BASE &&
      cmd <= (CMD_SET_HIGHER_COL_BASE | CMD_SET_HIGHER_COL_MASK)) {
    // ページアドレッシングモード: 列アドレス上位ニブル
    pageColHigh = static_cast<uint8_t>((cmd & CMD_SET_HIGHER_COL_MASK) << 4);
    applyPageModeCol();
    return;
  }

  switch (cmd) {
    case CMD_DISPLAY_OFF:
      displayOn = false;
      log("SSD1309: DISPLAY_OFF");
      break;
    case CMD_DISPLAY_ON:
      displayOn = true;
      log("SSD1309: DISPLAY_ON");
      break;
    case CMD_NORMAL_DISPLAY:
      inverted = config.invertInvPolarity;
      log("SSD1309: NORMAL_DISPLAY");
      break;
    case CMD_INVERT_DISPLAY:
      inverted = !config.invertInvPolarity;
      log("SSD1309: INVERT_DISPLAY");
      break;
    case CMD_SEG_REMAP_0:
      ssdSegmentRemap = false;
      log("SSD1309: SEG_REMAP_0");
      break;
    case CMD_SEG_REMAP_1:
      ssdSegmentRemap = true;
      log("SSD1309: SEG_REMAP_1");
      break;
    case CMD_COM_SCAN_INC:
      ssdComFlip = false;
      log("SSD1309: COM_SCAN_INC");
      break;
    case CMD_COM_SCAN_DEC:
      ssdComFlip = true;
      log("SSD1309: COM_SCAN_DEC");
      break;
    case CMD_SET_ADDR_MODE: expectedParams = 1; break;
    case CMD_SET_COL_ADDR: expectedParams = 2; break;
    case CMD_SET_PAGE_ADDR: expectedParams = 2; break;
    // 1パラメータのみで表示に影響しないコマンド
    case CMD_SET_CONTRAST:
    case CMD_SET_MULTIPLEX:
    case CMD_SET_DISPLAY_OFFSET:
    case CMD_SET_CLK_DIV:
    case CMD_SET_PRECHARGE:
    case CMD_SET_COM_PINS:
    case CMD_SET_VCOMH: expectedParams = 1; break;
    case CMD_NOP:
    default: break;
  }
}

// SSD1309 では DCX=1 は常に GDDRAM データ → feedDataByte は呼ばれない
void Ssd1309Controller::feedDataByte(uint8_t /*byte*/) {}

// DCX=1 は常に GDDRAM (pixel) データ
bool Ssd1309Controller::isRamWriteCommand() const { return true; }

// MONO_VPACK: 1バイト = 縦8ピクセル (bit0=上端, bit7=下端)
void Ssd1309Controller::processRamwrData(const uint8_t* data, size_t length) {
  const uint16_t lcdW = config.lcdWidth;
  const uint16_t lcdH = config.lcdHeight;

  for (size_t i = 0; i < length; ++i) {
    uint8_t byte = data[i];

    // 1バイト = 8行分のピクセルを縦方向に書き込む
    for (uint16_t bit = 0; bit < 8u; ++bit) {
      uint16_t row = static_cast<uint16_t>(ramwrY + bit);
      if (row < lcdH) {
        uint16_t physRow =
            ssdComFlip ? static_cast<uint16_t>(lcdH - 1u - row) : row;
        uint16_t physCol = ssdSegmentRemap
                               ? static_cast<uint16_t>(lcdW - 1u - ramwrX)
                               : ramwrX;
        framebuf[physRow * lcdW + physCol] =
            static_cast<uint16_t>((byte & 1u) ? 0xFFFFu : 0x0000u);
      }
      byte >>= 1;
    }

    // 書き込み位置を進める
    switch (ssdAddrMode) {
      case 0:  // 水平アドレッシング: 列→ページ
        if (++ramwrX > casetXE) {
          ramwrX = casetXS;
          ramwrY = static_cast<uint16_t>(ramwrY + 8u);
          if (ramwrY > rasetYE) ramwrY = rasetYS;
        }
        break;
      case 1:  // 垂直アドレッシング: ページ→列
        ramwrY = static_cast<uint16_t>(ramwrY + 8u);
        if (ramwrY > rasetYE) {
          ramwrY = rasetYS;
          if (++ramwrX > casetXE) ramwrX = casetXS;
        }
        break;
      default:  // ページアドレッシング: 列のみ
        if (++ramwrX > casetXE) ramwrX = casetXS;
        break;
    }
  }
}

void Ssd1309Controller::applyPageModeCol() {
  ramwrX = static_cast<uint16_t>(pageColHigh | pageColLow);
  if (ramwrX >= config.lcdWidth)
    ramwrX = static_cast<uint16_t>(config.lcdWidth - 1u);
}

}  // namespace lcdtap
