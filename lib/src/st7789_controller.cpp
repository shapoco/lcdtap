#include "st7789_controller.hpp"

namespace lcdtap {

uint16_t St7789Controller::logicalWidth() const {
  return ((madctl >> 5) & 1) ? config.lcdHeight : config.lcdWidth;
}

uint16_t St7789Controller::logicalHeight() const {
  return ((madctl >> 5) & 1) ? config.lcdWidth : config.lcdHeight;
}

void St7789Controller::updateWriteCache() {
  bool mv = (madctl >> 5) & 1;
  bool mx = (madctl >> 6) & 1;
  bool my = (madctl >> 7) & 1;
  cachedBGR = ((madctl >> 3) & 1) ^ config.swapRB;
  int32_t W = static_cast<int32_t>(config.lcdWidth);
  int32_t H = static_cast<int32_t>(config.lcdHeight);
  if (!mv) {
    cachedHOffset = mx ? (config.lcdWidth - 1) : 0;
    cachedHStep = mx ? -1 : +1;
    cachedVOffset = my ? (config.lcdHeight - 1) * W : 0;
    cachedVStep = my ? -W : +W;
  } else {
    cachedHOffset = my ? (config.lcdHeight - 1) * W : 0;
    cachedHStep = my ? -W : +W;
    cachedVOffset = mx ? (config.lcdWidth - 1) : 0;
    cachedVStep = mx ? -1 : +1;
  }
  if (framebuf) writePtr = framebuf + physIndex(ramwrX, ramwrY);
}

void St7789Controller::softReset() {
  madctl = 0;
  resetCommon();
}

void St7789Controller::dispatchCommand(uint8_t cmd) {
  using namespace st7789;
  currentCmd = cmd;
  cmdDataLen = 0;
  ramwrBufLen = 0;

  switch (cmd) {
    case CMD_NOP: break;
    case CMD_SWRESET:
      softReset();
      log("SWRESET");
      break;
    case CMD_SLPOUT:
      sleeping = false;
      log("SLPOUT");
      break;
    case CMD_INVOFF:
      inverted = config.invertInvPolarity;
      log("INVOFF");
      break;
    case CMD_INVON:
      inverted = !config.invertInvPolarity;
      log("INVON");
      break;
    case CMD_DISPON:
      displayOn = true;
      log("DISPON");
      break;
    case CMD_RAMWR:
      ramwrX = casetXS;
      ramwrY = rasetYS;
      ramwrBufLen = 0;
      updateWriteCache();
      break;
    // CMD_MADCTL / CMD_COLMOD / CMD_CASET / CMD_RASET wait for data bytes
    default: break;
  }
}

void St7789Controller::feedDataByte(uint8_t byte) {
  using namespace st7789;
  switch (currentCmd) {
    case CMD_MADCTL:
      if (cmdDataLen == 0) {
        madctl = byte;
        updateWriteCache();
        log("MADCTL");
      }
      ++cmdDataLen;
      break;
    case CMD_COLMOD:
      if (cmdDataLen == 0) {
        uint8_t fmt = byte & 0x07u;
        if (fmt == 0x03u)
          pixelFormat = PixelFormat::RGB444;
        else if (fmt == 0x05u)
          pixelFormat = PixelFormat::RGB565;
        else if (fmt == 0x06u)
          pixelFormat = PixelFormat::RGB666;
        ramwrBufLen = 0;  // reset buffer on format change
        log("COLMOD");
      }
      ++cmdDataLen;
      break;
    case CMD_CASET:
      switch (cmdDataLen) {
        case 0: ramwrBuf[0] = byte; break;
        case 1:
          casetXS = static_cast<uint16_t>((ramwrBuf[0] << 8) | byte);
          break;
        case 2: ramwrBuf[0] = byte; break;
        case 3:
          casetXE = static_cast<uint16_t>((ramwrBuf[0] << 8) | byte);
          log("CASET");
          break;
        default: break;
      }
      ++cmdDataLen;
      break;
    case CMD_RASET:
      switch (cmdDataLen) {
        case 0: ramwrBuf[0] = byte; break;
        case 1:
          rasetYS = static_cast<uint16_t>((ramwrBuf[0] << 8) | byte);
          break;
        case 2: ramwrBuf[0] = byte; break;
        case 3:
          rasetYE = static_cast<uint16_t>((ramwrBuf[0] << 8) | byte);
          log("RASET");
          break;
        default: break;
      }
      ++cmdDataLen;
      break;
    default: break;
  }
}

bool St7789Controller::isRamWriteCommand() const {
  return currentCmd == st7789::CMD_RAMWR;
}

}  // namespace lcdtap
