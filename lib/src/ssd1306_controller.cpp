#include "ssd1306_controller.hpp"

#include <lcdtap/devices/ssd1306.hpp>

namespace lcdtap {

uint16_t Ssd1306Controller::logicalWidth() const { return config.lcdWidth; }

uint16_t Ssd1306Controller::logicalHeight() const { return config.lcdHeight; }

void Ssd1306Controller::updateWriteCache() {
  cachedHOffset = 0;
  cachedHStep = 1;
  cachedVOffset = 0;
  cachedVStep = config.lcdWidth;
}

void Ssd1306Controller::softReset() {
  ssdAddrMode = 2;  // page addressing (SSD1306 default)
  ssdSegmentRemap = false;
  ssdComFlip = false;
  expectedParams = 0;
  pageColLow = 0;
  pageColHigh = 0;
  resetCommon();
  sleeping = false;  // SSD1306 has no sleep mode
  pixelFormat = PixelFormat::MONO_VPACK;
  casetXS = 0;
  casetXE = static_cast<uint16_t>(config.lcdWidth - 1);
  rasetYS = 0;
  rasetYE = static_cast<uint16_t>(config.lcdHeight - 1);
  ramwrX = 0;
  ramwrY = 0;
}

// Accepts all DCX=0 bytes (both command body and parameters)
void Ssd1306Controller::dispatchCommand(uint8_t cmd) {
  using namespace ssd1306;

  // Waiting for parameters of the current command
  if (expectedParams > 0) {
    --expectedParams;
    switch (currentCmd) {
      case CMD_SET_ADDR_MODE:
        ssdAddrMode = cmd & 0x03u;
        log("SSD1306: SET_ADDR_MODE");
        break;
      case CMD_SET_COL_ADDR:
        if (cmdDataLen == 0) {
          casetXS = cmd;
          ramwrX = casetXS;
        } else {
          casetXE = cmd;
          log("SSD1306: SET_COL_ADDR");
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
          log("SSD1306: SET_PAGE_ADDR");
        }
        break;
      default: break;  // parameter for a command that is otherwise ignored
    }
    ++cmdDataLen;
    return;
  }

  // New command
  currentCmd = cmd;
  cmdDataLen = 0;
  ramwrBufLen = 0;
  expectedParams = 0;

  if (cmd >= (CMD_SET_START_LINE_BASE) &&
      cmd <= (CMD_SET_START_LINE_BASE | CMD_SET_START_LINE_MASK)) {
    // Start-line (scroll): accepted but not applied
    return;
  }

  if (cmd >= CMD_SET_PAGE_START_BASE &&
      cmd <= (CMD_SET_PAGE_START_BASE | CMD_SET_PAGE_START_MASK)) {
    // Page addressing mode: set page
    uint8_t page = cmd & CMD_SET_PAGE_START_MASK;
    ramwrY = static_cast<uint16_t>(page * 8u);
    ramwrX = casetXS;
    return;
  }

  if (cmd >= CMD_SET_LOWER_COL_BASE &&
      cmd <= (CMD_SET_LOWER_COL_BASE | CMD_SET_LOWER_COL_MASK)) {
    // Page addressing mode: column address lower nibble
    pageColLow = cmd & CMD_SET_LOWER_COL_MASK;
    applyPageModeCol();
    return;
  }

  if (cmd >= CMD_SET_HIGHER_COL_BASE &&
      cmd <= (CMD_SET_HIGHER_COL_BASE | CMD_SET_HIGHER_COL_MASK)) {
    // Page addressing mode: column address upper nibble
    pageColHigh = static_cast<uint8_t>((cmd & CMD_SET_HIGHER_COL_MASK) << 4);
    applyPageModeCol();
    return;
  }

  switch (cmd) {
    case CMD_DISPLAY_OFF:
      displayOn = false;
      log("SSD1306: DISPLAY_OFF");
      break;
    case CMD_DISPLAY_ON:
      displayOn = true;
      log("SSD1306: DISPLAY_ON");
      break;
    case CMD_NORMAL_DISPLAY:
      inverted = config.invertInvPolarity;
      log("SSD1306: NORMAL_DISPLAY");
      break;
    case CMD_INVERT_DISPLAY:
      inverted = !config.invertInvPolarity;
      log("SSD1306: INVERT_DISPLAY");
      break;
    case CMD_SEG_REMAP_0:
      ssdSegmentRemap = false;
      log("SSD1306: SEG_REMAP_0");
      break;
    case CMD_SEG_REMAP_1:
      ssdSegmentRemap = true;
      log("SSD1306: SEG_REMAP_1");
      break;
    case CMD_COM_SCAN_INC:
      ssdComFlip = false;
      log("SSD1306: COM_SCAN_INC");
      break;
    case CMD_COM_SCAN_DEC:
      ssdComFlip = true;
      log("SSD1306: COM_SCAN_DEC");
      break;
    case CMD_SET_ADDR_MODE: expectedParams = 1; break;
    case CMD_SET_COL_ADDR: expectedParams = 2; break;
    case CMD_SET_PAGE_ADDR: expectedParams = 2; break;
    // Single-parameter commands with no visible effect
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

// For SSD1306, DCX=1 is always GDDRAM data — feedDataByte is never called
void Ssd1306Controller::feedDataByte(uint8_t /*byte*/) {}

// DCX=1 is always GDDRAM (pixel) data
bool Ssd1306Controller::isRamWriteCommand() const { return true; }

// MONO_VPACK: 1 byte = 8 vertical pixels (bit0=top, bit7=bottom)
void Ssd1306Controller::processRamwrData(const uint8_t* data, uint32_t length,
                                         uint32_t stride) {
  const uint16_t lcdW = config.lcdWidth;
  const uint16_t lcdH = config.lcdHeight;

  for (uint32_t i = 0; i < length; i += stride) {
    uint8_t byte = data[i];

    // Write 8 pixels vertically from 1 byte
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

    // Advance write position
    switch (ssdAddrMode) {
      case 0:  // horizontal addressing: column → page
        if (++ramwrX > casetXE) {
          ramwrX = casetXS;
          ramwrY = static_cast<uint16_t>(ramwrY + 8u);
          if (ramwrY > rasetYE) ramwrY = rasetYS;
        }
        break;
      case 1:  // vertical addressing: page → column
        ramwrY = static_cast<uint16_t>(ramwrY + 8u);
        if (ramwrY > rasetYE) {
          ramwrY = rasetYS;
          if (++ramwrX > casetXE) ramwrX = casetXS;
        }
        break;
      default:  // page addressing: column only
        if (++ramwrX > casetXE) ramwrX = casetXS;
        break;
    }
  }
}

void Ssd1306Controller::applyPageModeCol() {
  ramwrX = static_cast<uint16_t>(pageColHigh | pageColLow);
  if (ramwrX >= config.lcdWidth)
    ramwrX = static_cast<uint16_t>(config.lcdWidth - 1u);
}

}  // namespace lcdtap
