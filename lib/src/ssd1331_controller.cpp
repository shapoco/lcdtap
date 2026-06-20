#include "ssd1331_controller.hpp"

#include <algorithm>

#include <lcdtap/devices/ssd1331.hpp>

namespace lcdtap {

uint16_t Ssd1331Controller::logicalWidth() const {
  return (remap & ssd1331::REMAP_ADDR_INC) ? config.buffHeight : config.buffWidth;
}

uint16_t Ssd1331Controller::logicalHeight() const {
  return (remap & ssd1331::REMAP_ADDR_INC) ? config.buffWidth : config.buffHeight;
}

// Map SETREMAP bits to write-cache offsets and steps (mirrors ST7789 MADCTL
// logic)
void Ssd1331Controller::updateWriteCache() {
  bool mv = (remap & ssd1331::REMAP_ADDR_INC) != 0;   // vertical increment
  bool mx = (remap & ssd1331::REMAP_COL_REMAP) != 0;  // mirror X
  bool my = ((remap & ssd1331::REMAP_COM_REMAP) != 0) ^
            mv;  // mirror Y (inverted when MV=1)
  // Map hardware window coordinates to logical coordinates used by physIndex().
  // When mv=true the fast axis is the hardware row and the slow axis is the
  // hardware column, so they must be swapped relative to the mv=false case.
  if (!mv) {
    casetXS = hwColStart;
    casetXE = hwColEnd;
    rasetYS = hwRowStart;
    rasetYE = hwRowEnd;
  } else {
    casetXS = hwRowStart;  // fast axis = hardware row
    casetXE = hwRowEnd;
    rasetYS = hwColStart;  // slow axis = hardware col
    rasetYE = hwColEnd;
  }
  ramwrX = casetXS;
  ramwrY = rasetYS;
  cachedBGR = ((remap & ssd1331::REMAP_BGR) != 0) ^ config.swapRB;
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

void Ssd1331Controller::softReset() {
  remap = 0;
  hwColStart = 0;
  hwColEnd = static_cast<uint8_t>(config.buffWidth - 1);
  hwRowStart = 0;
  hwRowEnd = static_cast<uint8_t>(config.buffHeight - 1);
  fillEnabled = false;
  expectedParams = 0;
  cmdBufLen = 0;
  resetCommon();
  interfaceFormat = InterfaceFormat::RGB332;  // SSD1331 default: 8bpp RGB332
}

// Accepts all DCX=0 bytes (both command body and parameters)
void Ssd1331Controller::dispatchCommand(uint8_t cmd) {
  using namespace ssd1331;

  // Accumulate parameters for the current command
  if (expectedParams > 0) {
    cmdBuf[cmdBufLen++] = cmd;
    if (--expectedParams == 0) execCommand();
    return;
  }

  // New command
  currentCmd = cmd;
  cmdDataLen = 0;
  cmdBufLen = 0;
  ramwrBufLen = 0;

  switch (cmd) {
    case CMD_SETCOLUMN:
    case CMD_SETROW: expectedParams = 2; break;
    case CMD_DRAWLINE: expectedParams = 7; break;
    case CMD_DRAWRECT: expectedParams = 10; break;
    case CMD_COPY: expectedParams = 6; break;
    case CMD_DIMWINDOW:
    case CMD_CLEARWINDOW: expectedParams = 4; break;
    case CMD_FILLENABLE:
    case CMD_SETREMAP:
    case CMD_STARTLINE:
    case CMD_DISPLAYOFFSET:
    case CMD_MULTIPLEX:
    case CMD_SETMASTER:
    case CMD_POWERMODE:
    case CMD_PRECHARGE:
    case CMD_CLOCKDIV:
    case CMD_SETCONTRASTCCA:
    case CMD_SETCONTRASTCCB:
    case CMD_SETCONTRASTCCC:
    case CMD_MASTERCURRENT:
    case CMD_PRECHARGEA:
    case CMD_PRECHARGEB:
    case CMD_PRECHARGEC:
    case CMD_VCOMH: expectedParams = 1; break;
    case CMD_DISPLAYOFF:
      sleeping = true;
      log("SSD1331: DISPLAYOFF");
      break;
    case CMD_DISPLAYON:
      sleeping = false;
      displayOn = true;
      log("SSD1331: DISPLAYON");
      break;
    case CMD_DISPLAYALLON:
      sleeping = false;
      displayOn = true;
      break;
    case CMD_DISPLAYALLOFF: sleeping = true; break;
    case CMD_NORMALDISPLAY: inverted = config.inverted; break;
    case CMD_INVERTDISPLAY: inverted = !config.inverted; break;
    default: expectedParams = 0; break;
  }
}

void Ssd1331Controller::feedDataByte(uint8_t /*byte*/) {
  // no-op: all DCX=1 bytes go to GRAM via processRamwrData()
}

bool Ssd1331Controller::isRamWriteCommand() const { return true; }

// Called when all parameters for the current command have been accumulated
void Ssd1331Controller::execCommand() {
  using namespace ssd1331;

  switch (currentCmd) {
    case CMD_SETCOLUMN:
      if ((remap & ssd1331::REMAP_ADDR_INC) == 0) {
        // mv=0: SETCOLUMN = column address, clip to lcdWidth
        hwColStart = LCDTAP_CLIP(0, config.buffWidth - 1, cmdBuf[0]);
        hwColEnd = LCDTAP_CLIP(hwColStart, config.buffWidth - 1, cmdBuf[1]);
        expandTrimX(hwColStart, hwColEnd);
      } else {
        // mv=1: SETCOLUMN = row address, clip to lcdHeight
        hwRowStart = LCDTAP_CLIP(0, config.buffHeight - 1, cmdBuf[0]);
        hwRowEnd = LCDTAP_CLIP(hwRowStart, config.buffHeight - 1, cmdBuf[1]);
        expandTrimY(hwRowStart, hwRowEnd);
      }
      // casetXS/XE and ramwrX are set in updateWriteCache() when CMD_SETROW
      // arrives
      log("SSD1331: SETCOLUMN");
      break;

    case CMD_SETROW:
      if ((remap & ssd1331::REMAP_ADDR_INC) == 0) {
        // mv=0: SETROW = row address, clip to lcdHeight
        hwRowStart = LCDTAP_CLIP(0, config.buffHeight - 1, cmdBuf[0]);
        hwRowEnd = LCDTAP_CLIP(hwRowStart, config.buffHeight - 1, cmdBuf[1]);
        expandTrimY(hwRowStart, hwRowEnd);
      } else {
        // mv=1: SETROW = column address, clip to lcdWidth
        hwColStart = LCDTAP_CLIP(0, config.buffWidth - 1, cmdBuf[0]);
        hwColEnd = LCDTAP_CLIP(hwColStart, config.buffWidth - 1, cmdBuf[1]);
        expandTrimX(hwColStart, hwColEnd);
      }
      ramwrBufLen = 0;
      updateWriteCache();
      log("SSD1331: SETROW");
      break;

    case CMD_SETREMAP: {
      remap = cmdBuf[0];
      // Update pixel format from color depth bits
      uint8_t depth = remap & REMAP_COLOR_DEPTH_MASK;
      if (depth == REMAP_COLOR_DEPTH_256)
        interfaceFormat = InterfaceFormat::RGB332;
      else
        interfaceFormat = InterfaceFormat::RGB565_BE;
      ramwrBufLen = 0;
      updateWriteCache();
      log("SSD1331: SETREMAP");
      break;
    }

    case CMD_FILLENABLE: fillEnabled = (cmdBuf[0] & 0x01u) != 0; break;

    case CMD_DRAWLINE: {
      uint16_t color = makeColor(cmdBuf[4], cmdBuf[5], cmdBuf[6]);
      drawLine(cmdBuf[0], cmdBuf[1], cmdBuf[2], cmdBuf[3], color);
      log("SSD1331: DRAWLINE");
      break;
    }

    case CMD_DRAWRECT: {
      int16_t x0 = cmdBuf[0], y0 = cmdBuf[1], x1 = cmdBuf[2], y1 = cmdBuf[3];
      uint16_t borderColor = makeColor(cmdBuf[4], cmdBuf[5], cmdBuf[6]);
      uint16_t fillColor = makeColor(cmdBuf[7], cmdBuf[8], cmdBuf[9]);
      if (fillEnabled) fillRect(x0, y0, x1, y1, fillColor);
      // Draw 4 edges on top
      drawLine(x0, y0, x1, y0, borderColor);  // top
      drawLine(x0, y1, x1, y1, borderColor);  // bottom
      drawLine(x0, y0, x0, y1, borderColor);  // left
      drawLine(x1, y0, x1, y1, borderColor);  // right
      log("SSD1331: DRAWRECT");
      break;
    }

    case CMD_COPY:
      copyRegion(cmdBuf[0], cmdBuf[1], cmdBuf[2], cmdBuf[3], cmdBuf[4],
                 cmdBuf[5]);
      log("SSD1331: COPY");
      break;

    case CMD_DIMWINDOW:
      dimWindow(cmdBuf[0], cmdBuf[1], cmdBuf[2], cmdBuf[3]);
      log("SSD1331: DIMWINDOW");
      break;

    case CMD_CLEARWINDOW:
      fillRect(cmdBuf[0], cmdBuf[1], cmdBuf[2], cmdBuf[3], 0x0000u);
      log("SSD1331: CLEARWINDOW");
      break;

    default: break;
  }
}

// Convert 6-bit-per-channel color parameters to RGB565
uint16_t Ssd1331Controller::makeColor(uint8_t r6, uint8_t g6,
                                      uint8_t b6) const {
  uint16_t r5 = (r6 >> 1) & 0x1Fu;
  uint16_t g = g6 & 0x3Fu;
  uint16_t b5 = (b6 >> 1) & 0x1Fu;
  if (cachedBGR) {
    return static_cast<uint16_t>((b5 << 11) | (g << 5) | r5);
  }
  return static_cast<uint16_t>((r5 << 11) | (g << 5) | b5);
}

void Ssd1331Controller::setPixelAt(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || x >= static_cast<int16_t>(logicalWidth())) return;
  if (y < 0 || y >= static_cast<int16_t>(logicalHeight())) return;
  framebuf[physIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y))] =
      color;
}

// Bresenham line algorithm
void Ssd1331Controller::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                 uint16_t color) {
  int16_t dx = static_cast<int16_t>(x1 > x0 ? x1 - x0 : x0 - x1);
  int16_t dy = static_cast<int16_t>(y1 > y0 ? y1 - y0 : y0 - y1);
  int16_t sx = (x0 < x1) ? 1 : -1;
  int16_t sy = (y0 < y1) ? 1 : -1;
  int16_t err = dx - dy;
  while (true) {
    setPixelAt(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    int16_t e2 = static_cast<int16_t>(err * 2);
    if (e2 > -dy) {
      err = static_cast<int16_t>(err - dy);
      x0 = static_cast<int16_t>(x0 + sx);
    }
    if (e2 < dx) {
      err = static_cast<int16_t>(err + dx);
      y0 = static_cast<int16_t>(y0 + sy);
    }
  }
}

// Fill rectangle; fast path when pixels are contiguous (cachedHStep == 1)
void Ssd1331Controller::fillRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                 uint16_t color) {
  int16_t lw = static_cast<int16_t>(logicalWidth());
  int16_t lh = static_cast<int16_t>(logicalHeight());
  if (x0 > x1) {
    int16_t t = x0;
    x0 = x1;
    x1 = t;
  }
  if (y0 > y1) {
    int16_t t = y0;
    y0 = y1;
    y1 = t;
  }
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= lw) x1 = static_cast<int16_t>(lw - 1);
  if (y1 >= lh) y1 = static_cast<int16_t>(lh - 1);
  if (x0 > x1 || y0 > y1) return;

  int16_t w = static_cast<int16_t>(x1 - x0 + 1);
  for (int16_t y = y0; y <= y1; ++y) {
    int32_t idx = static_cast<int32_t>(
        physIndex(static_cast<uint32_t>(x0), static_cast<uint32_t>(y)));
    if (cachedHStep == 1) {
      std::fill(framebuf + idx, framebuf + idx + w, color);
    } else {
      for (int16_t x = 0; x < w; ++x, idx += cachedHStep) framebuf[idx] = color;
    }
  }
}

// Halve the brightness of each pixel in the window
void Ssd1331Controller::dimWindow(int16_t x0, int16_t y0, int16_t x1,
                                  int16_t y1) {
  int16_t lw = static_cast<int16_t>(logicalWidth());
  int16_t lh = static_cast<int16_t>(logicalHeight());
  if (x0 > x1) {
    int16_t t = x0;
    x0 = x1;
    x1 = t;
  }
  if (y0 > y1) {
    int16_t t = y0;
    y0 = y1;
    y1 = t;
  }
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= lw) x1 = static_cast<int16_t>(lw - 1);
  if (y1 >= lh) y1 = static_cast<int16_t>(lh - 1);
  if (x0 > x1 || y0 > y1) return;

  int16_t w = static_cast<int16_t>(x1 - x0 + 1);
  for (int16_t y = y0; y <= y1; ++y) {
    int32_t idx = static_cast<int32_t>(
        physIndex(static_cast<uint32_t>(x0), static_cast<uint32_t>(y)));
    for (int16_t x = 0; x < w; ++x, idx += cachedHStep) {
      uint16_t px = framebuf[idx];
      uint16_t r5 = (px >> 11) & 0x1Fu;
      uint16_t g6 = (px >> 5) & 0x3Fu;
      uint16_t b5 = px & 0x1Fu;
      framebuf[idx] = static_cast<uint16_t>(((r5 >> 1) << 11) |
                                            ((g6 >> 1) << 5) | (b5 >> 1));
    }
  }
}

// Copy source rectangle to destination; does not handle overlapping regions
void Ssd1331Controller::copyRegion(int16_t x0, int16_t y0, int16_t x1,
                                   int16_t y1, int16_t xd, int16_t yd) {
  int16_t lw = static_cast<int16_t>(logicalWidth());
  int16_t lh = static_cast<int16_t>(logicalHeight());
  if (x0 > x1) {
    int16_t t = x0;
    x0 = x1;
    x1 = t;
  }
  if (y0 > y1) {
    int16_t t = y0;
    y0 = y1;
    y1 = t;
  }

  int16_t w = static_cast<int16_t>(x1 - x0 + 1);
  int16_t h = static_cast<int16_t>(y1 - y0 + 1);

  for (int16_t dy = 0; dy < h; ++dy) {
    int16_t ys = static_cast<int16_t>(y0 + dy);
    int16_t ydst = static_cast<int16_t>(yd + dy);
    if (ys < 0 || ys >= lh || ydst < 0 || ydst >= lh) continue;

    int32_t src = static_cast<int32_t>(
        physIndex(static_cast<uint32_t>(x0), static_cast<uint32_t>(ys)));
    int32_t dst = static_cast<int32_t>(
        physIndex(static_cast<uint32_t>(xd), static_cast<uint32_t>(ydst)));

    if (cachedHStep == 1) {
      int16_t xs_clamped = x0 < 0 ? 0 : x0;
      int16_t xd_clamped = xd < 0 ? 0 : xd;
      int16_t w_clamped = w;
      if (xs_clamped > x0) {
        int16_t skip = static_cast<int16_t>(xs_clamped - x0);
        src += skip;
        dst += skip;
        w_clamped = static_cast<int16_t>(w_clamped - skip);
      }
      if (xd_clamped > xd) {
        int16_t skip = static_cast<int16_t>(xd_clamped - xd);
        src += skip;
        dst += skip;
        w_clamped = static_cast<int16_t>(w_clamped - skip);
      }
      int16_t xe_s = static_cast<int16_t>(xs_clamped + w_clamped - 1);
      int16_t xe_d = static_cast<int16_t>(xd_clamped + w_clamped - 1);
      if (xe_s >= lw)
        w_clamped = static_cast<int16_t>(w_clamped - (xe_s - lw + 1));
      if (xe_d >= lw)
        w_clamped = static_cast<int16_t>(w_clamped - (xe_d - lw + 1));
      if (w_clamped <= 0) continue;
      std::copy(framebuf + src, framebuf + src + w_clamped, framebuf + dst);
    } else {
      for (int16_t dx = 0; dx < w; ++dx) {
        int16_t xs = static_cast<int16_t>(x0 + dx);
        int16_t xdst = static_cast<int16_t>(xd + dx);
        if (xs < 0 || xs >= lw || xdst < 0 || xdst >= lw) {
          src += cachedHStep;
          dst += cachedHStep;
          continue;
        }
        framebuf[dst] = framebuf[src];
        src += cachedHStep;
        dst += cachedHStep;
      }
    }
  }
}

}  // namespace lcdtap
