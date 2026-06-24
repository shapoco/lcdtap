#include "spi_display_base.hpp"

namespace lcdtap {

static constexpr uint8_t CMD_NOP = 0x00;
static constexpr uint8_t CMD_SWRESET = 0x01;
static constexpr uint8_t CMD_SLPOUT = 0x11;
static constexpr uint8_t CMD_INVOFF = 0x20;
static constexpr uint8_t CMD_INVON = 0x21;
static constexpr uint8_t CMD_DISPON = 0x29;
static constexpr uint8_t CMD_CASET = 0x2A;
static constexpr uint8_t CMD_RASET = 0x2B;
static constexpr uint8_t CMD_RAMWR = 0x2C;
static constexpr uint8_t CMD_MADCTL = 0x36;
static constexpr uint8_t CMD_COLMOD = 0x3A;

uint16_t SpiDisplayBase::logicalWidth() const {
  return (madctl & MADCTL_MV) ? config.buffHeight : config.buffWidth;
}

uint16_t SpiDisplayBase::logicalHeight() const {
  return (madctl & MADCTL_MV) ? config.buffWidth : config.buffHeight;
}

void SpiDisplayBase::updateWriteCache() {
  bool mv = !!(madctl & MADCTL_MV);
  uint8_t effM = effectiveMadctl();
  bool mx = !!(effM & MADCTL_MX);
  bool my = !!(effM & MADCTL_MY);
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
  if (frameBuffer) {
    writePtr = frameBuffer + physIndex(ramwrX, ramwrY);
  }
}

void SpiDisplayBase::softReset() {
  madctl = 0;
  hwColStart = 0;
  hwColEnd = config.buffWidth - 1;
  hwRowStart = 0;
  hwRowEnd = config.buffHeight - 1;
  resetCommon();
}

void SpiDisplayBase::dispatchCommand(uint8_t cmd) {
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
      setInverted(config.inverted);
      log("INVOFF");
      break;
    case CMD_INVON:
      setInverted(!config.inverted);
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
      if (frameBuffer) {
        writePtr = frameBuffer + physIndex(ramwrX, ramwrY);
      }
      break;
    // CMD_MADCTL / CMD_COLMOD / CMD_CASET / CMD_RASET wait for data bytes
    default: onDispatchCommand(cmd); break;
  }
}

void SpiDisplayBase::feedDataByte(uint8_t byte) {
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
        InterfaceFormat fmt = colmodToFormat(byte & 0x07u);
        if (fmt != InterfaceFormat::NUM_FORMATS) interfaceFormat = fmt;
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
            if (effectiveMadctl() & MADCTL_MX) {
              uint16_t w = config.buffWidth;
              expandTrimX(w - hwColEnd - 1, w - hwColStart - 1);
            } else {
              expandTrimX(hwColStart, hwColEnd);
            }
          } else {
            hwRowEnd = LCDTAP_CLIP(hwRowStart, config.buffHeight - 1, val);
            if (effectiveMadctl() & MADCTL_MY) {
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
            if (effectiveMadctl() & MADCTL_MY) {
              uint16_t h = config.buffHeight;
              expandTrimY(h - hwRowEnd - 1, h - hwRowStart - 1);
            } else {
              expandTrimY(hwRowStart, hwRowEnd);
            }
          } else {
            hwColEnd = LCDTAP_CLIP(hwColStart, config.buffWidth - 1, val);
            if (effectiveMadctl() & MADCTL_MX) {
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
    default: onFeedDataByte(byte); break;
  }
}

bool SpiDisplayBase::isRamWriteCommand() const {
  return currentCmd == CMD_RAMWR;
}

InterfaceFormat SpiDisplayBase::colmodToFormat(uint8_t fmt) const {
  if (fmt == 0x05u) return InterfaceFormat::RGB565_BE;
  if (fmt == 0x06u) return InterfaceFormat::RGB666_UNPACK_LA8_BE;
  return InterfaceFormat::NUM_FORMATS;
}

}  // namespace lcdtap
