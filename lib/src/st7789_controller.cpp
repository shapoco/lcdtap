#include "st7789_controller.hpp"

namespace lcdtap {

static constexpr uint8_t MADCTL_BGR = 0x08;
static constexpr uint8_t MADCTL_MV = 0x20;
static constexpr uint8_t MADCTL_MX = 0x40;
static constexpr uint8_t MADCTL_MY = 0x80;

uint16_t St7789Controller::logicalWidth() const {
  return (madctl & MADCTL_MV) ? config.buffHeight : config.buffWidth;
}

uint16_t St7789Controller::logicalHeight() const {
  return (madctl & MADCTL_MV) ? config.buffWidth : config.buffHeight;
}

void St7789Controller::updateWriteCache() {
  bool mv = !!(madctl & MADCTL_MV);
  bool mx = !!(madctl & MADCTL_MX);
  bool my = !!(madctl & MADCTL_MY);
  // Map hardware window coordinates to logical coordinates used by physIndex().
  // When mv=true the fast axis is the hardware row and the slow axis is the
  // hardware column, so they must be swapped relative to the mv=false case.
  if (!mv) {
    casetXS = hwColStart;
    casetXE = hwColEnd;
    rasetYS = hwRowStart;
    rasetYE = hwRowEnd;
  } else {
    casetXS = hwRowStart;
    casetXE = hwRowEnd;
    rasetYS = hwColStart;
    rasetYE = hwColEnd;
  }
  cachedBGR = !!(madctl & MADCTL_BGR) ^ config.swapRB;
  int32_t W = static_cast<int32_t>(config.buffWidth);
  int32_t H = static_cast<int32_t>(config.buffHeight);
  if (!mv) {
    cachedHOffset = mx ? (config.buffWidth - 1) : 0;
    cachedHStep = mx ? -1 : +1;
    cachedVOffset = my ? (config.buffHeight - 1) * W : 0;
    cachedVStep = my ? -W : +W;
  } else {
    cachedHOffset = my ? (config.buffHeight - 1) * W : 0;
    cachedHStep = my ? -W : +W;
    cachedVOffset = mx ? (config.buffWidth - 1) : 0;
    cachedVStep = mx ? -1 : +1;
  }
  if (framebuf) writePtr = framebuf + physIndex(ramwrX, ramwrY);
}

void St7789Controller::softReset() {
  madctl = 0;
  hwColStart = 0;
  hwColEnd = config.buffWidth - 1;
  hwRowStart = 0;
  hwRowEnd = config.buffHeight - 1;
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
      inverted = config.inverted;
      log("INVOFF");
      break;
    case CMD_INVON:
      inverted = !config.inverted;
      log("INVON");
      break;
    case CMD_DISPON:
      displayOn = true;
      log("DISPON");
      break;
    case CMD_RAMWR:
      ramwrBufLen = 0;
      updateWriteCache();  // remaps hw→logical casetXS/rasetYS first
      ramwrX = casetXS;    // then reset write position to logical start
      ramwrY = rasetYS;
      if (framebuf) writePtr = framebuf + physIndex(ramwrX, ramwrY);
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
    case CMD_RAMCTRL:
      if (cmdDataLen ==
          1) {  // P2: bit 3 = ENDIAN (0 = big-endian, 1 = little-endian)
        cachedLittleEndian = (byte >> 3) & 1u;
      }
      ++cmdDataLen;
      break;
    case CMD_COLMOD:
      if (cmdDataLen == 0) {
        uint8_t fmt = byte & 0x07u;
        if (fmt == 0x01u)
          interfaceFormat = InterfaceFormat::RGB111_HPACK2_H2L_RA8;
        else if (fmt == 0x03u)
          interfaceFormat = InterfaceFormat::RGB444_HPACK2_H2L_BE;
        else if (fmt == 0x05u)
          interfaceFormat = InterfaceFormat::RGB565_BE;
        else if (fmt == 0x06u)
          interfaceFormat = InterfaceFormat::RGB666_UNPACK_LA8_BE;
        ramwrBufLen = 0;  // reset buffer on format change
        log("COLMOD");
      }
      ++cmdDataLen;
      break;
    case CMD_CASET:
      switch (cmdDataLen) {
        case 0: ramwrBuf[0] = byte; break;
        case 1: {
          uint16_t val = static_cast<uint16_t>((ramwrBuf[0] << 8) | byte);
          if (!(madctl & MADCTL_MV)) {
            hwColStart = LCDTAP_CLIP(0, config.buffWidth - 1, val);
          } else {
            hwRowStart = LCDTAP_CLIP(0, config.buffHeight - 1, val);
          }
        } break;
        case 2: ramwrBuf[0] = byte; break;
        case 3: {
          uint16_t val = static_cast<uint16_t>((ramwrBuf[0] << 8) | byte);
          if (!(madctl & MADCTL_MV)) {
            hwColEnd = LCDTAP_CLIP(hwColStart, config.buffWidth - 1, val);
            if (madctl & MADCTL_MX) {
              uint16_t w = config.buffWidth;
              expandTrimX(w - hwColEnd - 1, w - hwColStart - 1);
            } else {
              expandTrimX(hwColStart, hwColEnd);
            }
          } else {
            hwRowEnd = LCDTAP_CLIP(hwRowStart, config.buffHeight - 1, val);
            if (madctl & MADCTL_MY) {
              uint16_t h = config.buffHeight;
              expandTrimY(h - hwRowEnd - 1, h - hwRowStart - 1);
            } else {
              expandTrimY(hwRowStart, hwRowEnd);
            }
          }
        } break;
        default: break;
      }
      ++cmdDataLen;
      break;
    case CMD_RASET:
      switch (cmdDataLen) {
        case 0: ramwrBuf[0] = byte; break;
        case 1: {
          uint16_t val = static_cast<uint16_t>((ramwrBuf[0] << 8) | byte);
          if (!(madctl & MADCTL_MV)) {
            hwRowStart = LCDTAP_CLIP(0, config.buffHeight - 1, val);
          } else {
            hwColStart = LCDTAP_CLIP(0, config.buffWidth - 1, val);
          }
        } break;
        case 2: ramwrBuf[0] = byte; break;
        case 3: {
          uint16_t val = static_cast<uint16_t>((ramwrBuf[0] << 8) | byte);
          if (!(madctl & MADCTL_MV)) {
            hwRowEnd = LCDTAP_CLIP(hwRowStart, config.buffHeight - 1, val);
            if (madctl & MADCTL_MY) {
              uint16_t h = config.buffHeight;
              expandTrimY(h - hwRowEnd - 1, h - hwRowStart - 1);
            } else {
              expandTrimY(hwRowStart, hwRowEnd);
            }
          } else {
            hwColEnd = LCDTAP_CLIP(hwColStart, config.buffWidth - 1, val);
            if (madctl & MADCTL_MX) {
              uint16_t w = config.buffWidth;
              expandTrimX(w - hwColEnd - 1, w - hwColStart - 1);
            } else {
              expandTrimX(hwColStart, hwColEnd);
            }
          }
        } break;
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
