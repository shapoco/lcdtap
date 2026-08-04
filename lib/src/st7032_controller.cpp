#include "st7032_controller.hpp"

#include <cstring>

#include <lcdtap/devices/st7032.hpp>
#include <lcdtap/font_st7032.hpp>

namespace lcdtap {

uint16_t St7032Controller::logicalWidth() const { return config.buffWidth; }

uint16_t St7032Controller::logicalHeight() const { return config.buffHeight; }

void St7032Controller::updateWriteCache() {
  cachedHOffset = 0;
  cachedHStep = 1;
  cachedVOffset = 0;
  cachedVStep = config.buffWidth;
}

void St7032Controller::softReset() {
  memset(ddram, 0x20, sizeof(ddram));
  memset(cgram, 0x00, sizeof(cgram));
  ac = 0;
  acInCgram = false;
  shift = 0;
  entryInc = true;
  entryShift = false;
  dispC = false;
  dispB = false;
  funcDL = true;
  funcN = false;
  funcDH = false;
  funcIS = false;
  nibblePending = false;
  nibbleHigh = 0;
  nibbleIsData = false;
  blinkPhase = false;
  lastTickMs = 0;
  cursorCol = -1;
  cursorRow = -1;
  resetCommon();
  sleeping = false;  // ST7032 has no sleep state; only the D bit gates output
  setInverted(config.inverted);
  expandTrimX(0, static_cast<uint16_t>(config.buffWidth - 1));
  expandTrimY(0, static_cast<uint16_t>(config.buffHeight - 1));
  renderAll();
}

// True while the host talks in 4-bit mode: two bytes carry one value in
// their upper nibbles (D[7:4]), high nibble first. Only meaningful on the
// parallel bus; I2C and serial transfers are always 8-bit.
bool St7032Controller::nibbleMode() const {
  return !funcDL && config.busInterface == BusType::PARALLEL;
}

void St7032Controller::dispatchCommand(uint8_t cmd) {
  if (nibbleMode()) {
    if (nibblePending && !nibbleIsData) {
      nibblePending = false;
      execCommand(static_cast<uint8_t>(nibbleHigh | (cmd >> 4)));
    } else {
      // Also lands here when a data-nibble was pending: resync on RS change.
      nibblePending = true;
      nibbleHigh = cmd & 0xF0u;
      nibbleIsData = false;
    }
  } else {
    execCommand(cmd);
  }
}

void St7032Controller::execCommand(uint8_t cmd) {
  using namespace st7032;

  if (cmd >= CMD_SET_DDRAM_ADDR) {
    ac = cmd & DDRAM_ADDR_MASK;
    acInCgram = false;
    updateCursor();
    return;
  }

  if (cmd >= CMD_SET_CGRAM_ADDR) {
    if (funcIS) return;  // instruction table 1: ignored
    ac = cmd & CGRAM_ADDR_MASK;
    acInCgram = true;
    updateCursor();  // hides the cursor while AC is in CGRAM
    return;
  }

  if (cmd >= CMD_FUNCTION_SET) {
    bool newN = (cmd & FUNC_N) != 0;
    bool newDH = (cmd & FUNC_DH) != 0;
    bool layoutChanged = (newN != funcN) || (newDH != funcDH);
    funcDL = (cmd & FUNC_DL) != 0;
    funcN = newN;
    funcDH = newDH;
    funcIS = (cmd & FUNC_IS) != 0;
    nibblePending = false;
    if (layoutChanged) {
      shift = static_cast<uint8_t>(shift % (funcN ? 40u : 80u));
      renderAll();
    }
    log("ST7032: FUNCTION_SET");
    return;
  }

  if (cmd >= CMD_SHIFT) {
    if (funcIS) return;  // instruction table 1: ignored
    bool right = (cmd & SHIFT_RL) != 0;
    if (cmd & SHIFT_SC) {
      // Display shift: position 1 shows address (shift) — shifting left
      // increments, shifting right decrements (see datasheet Figure 9/11).
      uint8_t mod = funcN ? 40u : 80u;
      shift = static_cast<uint8_t>((shift + (right ? mod - 1u : 1u)) % mod);
      renderAll();
    } else {
      advanceAc(right);
      updateCursor();
    }
    return;
  }

  if (cmd >= CMD_DISPLAY_ONOFF) {
    bool d = (cmd & DISPLAY_D) != 0;
    dispC = (cmd & DISPLAY_C) != 0;
    dispB = (cmd & DISPLAY_B) != 0;
    if (!dispB) blinkPhase = false;
    if (d != displayOn) {
      displayOn = d;
      bumpEpoch();
    }
    updateCursor();
    log("ST7032: DISPLAY_ONOFF");
    return;
  }

  if (cmd >= CMD_ENTRY_MODE) {
    entryInc = (cmd & ENTRY_ID) != 0;
    entryShift = (cmd & ENTRY_S) != 0;
    return;
  }

  if (cmd >= CMD_RETURN_HOME) {
    ac = 0;
    acInCgram = false;
    shift = 0;
    renderAll();
    log("ST7032: RETURN_HOME");
    return;
  }

  if (cmd == CMD_CLEAR_DISPLAY) {
    memset(ddram, 0x20, sizeof(ddram));
    ac = 0;
    acInCgram = false;
    shift = 0;
    entryInc = true;
    renderAll();
    log("ST7032: CLEAR_DISPLAY");
    return;
  }

  noteUnknownCommand(cmd);
}

// RS=1 bytes are always DDRAM/CGRAM data
bool St7032Controller::isRamWriteCommand() const { return true; }

void St7032Controller::feedDataByte(uint8_t byte) { writeData(byte); }

void St7032Controller::processRamwrData(const uint8_t* data, uint32_t numBytes,
                                        uint32_t stride) {
  const uint32_t byteLen = numBytes * stride;
  for (uint32_t i = 0; i < byteLen; i += stride) {
    uint8_t byte = data[i];
    if (nibbleMode()) {
      if (nibblePending && nibbleIsData) {
        nibblePending = false;
        writeData(static_cast<uint8_t>(nibbleHigh | (byte >> 4)));
      } else {
        nibblePending = true;
        nibbleHigh = byte & 0xF0u;
        nibbleIsData = true;
      }
    } else {
      writeData(byte);
    }
  }
}

void St7032Controller::writeData(uint8_t byte) {
  if (acInCgram) {
    uint8_t idx = ac & (CGRAM_SIZE - 1u);
    cgram[idx] = byte & 0x1Fu;
    advanceAc(entryInc);
    renderCellsForCgramChar(idx >> 3);
    return;
  }

  int16_t idx = ddramIndex(ac);
  if (idx >= 0) ddram[idx] = byte;
  advanceAc(entryInc);
  if (entryShift) {
    // S=1: the display shifts along with the cursor on every DDRAM write
    uint8_t mod = funcN ? 40u : 80u;
    shift = static_cast<uint8_t>((shift + (entryInc ? 1u : mod - 1u)) % mod);
    renderAll();
  } else {
    if (idx >= 0) renderCellForDdramIndex(static_cast<uint16_t>(idx));
    updateCursor();
  }
}

void St7032Controller::advanceAc(bool inc) {
  if (acInCgram) {
    ac = static_cast<uint8_t>((ac + (inc ? 1u : CGRAM_SIZE - 1u)) &
                              (CGRAM_SIZE - 1u));
    return;
  }
  if (funcN) {
    // 2-line mode: 0x27 -> 0x40, 0x67 -> 0x00 (and the reverse on decrement)
    if (inc) {
      if (ac == 0x27)
        ac = 0x40;
      else if (ac == 0x67)
        ac = 0x00;
      else
        ac = (ac + 1u) & 0x7Fu;
    } else {
      if (ac == 0x40)
        ac = 0x27;
      else if (ac == 0x00)
        ac = 0x67;
      else
        ac = (ac - 1u) & 0x7Fu;
    }
  } else {
    // 1-line mode: 0x00..0x4F contiguous
    if (inc) {
      ac = (ac == 0x4F) ? 0u : ((ac + 1u) & 0x7Fu);
    } else {
      ac = (ac == 0x00) ? 0x4Fu : ((ac - 1u) & 0x7Fu);
    }
  }
}

// ddram[] index for a DDRAM address, or -1 if the address is unmapped
int16_t St7032Controller::ddramIndex(uint8_t addr) const {
  if (funcN) {
    if (addr <= 0x27) return addr;
    if (addr >= 0x40 && addr <= 0x67) return 40 + (addr - 0x40);
    return -1;
  }
  return (addr <= 0x4F) ? addr : -1;
}

// Visible cell (visCol, visRow) for a ddram[] index, or false if hidden.
// Layout (see the class comment): rows=4 folds each logical line in half,
// left half on rows 0/1 and right half on rows 2/3.
bool St7032Controller::cellFromDdramIndex(uint16_t index, uint16_t* visCol,
                                          uint16_t* visRow) const {
  const uint16_t cols = config.textCols;
  const uint16_t rows = config.textRows;
  uint16_t line, pos;
  if (funcN) {
    line = (index >= 40) ? 1u : 0u;
    pos = static_cast<uint16_t>((index % 40u + 40u - shift) % 40u);
    if (line == 1 && rows < 2) return false;
  } else {
    line = 0;
    pos = static_cast<uint16_t>((index + 80u - shift) % 80u);
  }
  if (pos < cols) {
    *visCol = pos;
    *visRow = line;
    return true;
  }
  if (rows == 4 && pos < cols * 2u) {
    *visCol = static_cast<uint16_t>(pos - cols);
    *visRow = static_cast<uint16_t>(line + 2u);
    return true;
  }
  return false;
}

// ddram[] index shown at a visible cell, or -1 if the cell is blank
int16_t St7032Controller::ddramIndexAtCell(uint16_t visCol,
                                           uint16_t visRow) const {
  const uint16_t cols = config.textCols;
  const uint16_t posOff = (visRow >= 2) ? cols : 0u;
  if (funcN) {
    uint16_t line = (visRow == 1u || visRow == 3u) ? 1u : 0u;
    uint16_t pos = static_cast<uint16_t>((visCol + posOff + shift) % 40u);
    return static_cast<int16_t>(line * 40u + pos);
  }
  // 1-line mode: only the first logical line exists
  if (visRow == 1u || visRow == 3u) return -1;
  return static_cast<int16_t>((visCol + posOff + shift) % 80u);
}

const uint8_t* St7032Controller::glyphFor(uint8_t code) const {
  // OPR2:OPR1 option selects how many codes the CGRAM replaces:
  // 0: 0x00-0x0F (8 chars mirrored), 1: 0x00-0x07, 2: 0x00-0x05, 3: none
  static constexpr uint8_t CGRAM_CODE_LIMIT[4] = {0x10, 0x08, 0x06, 0x00};
  if (code < CGRAM_CODE_LIMIT[config.textCgramArea & 3u]) {
    return &cgram[(code & 7u) * 8u];
  }
  return &font_st7032::bitmap[static_cast<uint32_t>(code) * 8u];
}

void St7032Controller::putPixel(uint16_t x, uint16_t y, bool on) {
  const uint16_t w = config.buffWidth;
  const uint8_t flip = static_cast<uint8_t>(config.flipMode);
  if (flip & 0x01u) x = static_cast<uint16_t>(w - 1u - x);
  if (flip & 0x02u) y = static_cast<uint16_t>(config.buffHeight - 1u - y);
  frameBuffer[y * w + x] = (on ? 0xFFFFu : 0x0000u) ^ cachedInverter;
}

// Render one character cell (glyph + trailing gaps) into the framebuffer.
// In double-height mode (N=0, DH=1, rows>=2) the cells of visible rows 0/2
// cover two rows' worth of height and rows 1/3 must not be drawn separately.
void St7032Controller::renderCell(uint16_t visCol, uint16_t visRow) {
  const uint16_t w = config.buffWidth;
  const uint16_t h = config.buffHeight;
  const bool dh = !funcN && funcDH && config.textRows >= 2;
  if (dh && (visRow == 1u || visRow == 3u)) return;

  const uint16_t x0 = static_cast<uint16_t>(visCol * CELL_W);
  const uint16_t y0 = dh ? static_cast<uint16_t>((visRow >> 1) * CELL_H * 2u)
                         : static_cast<uint16_t>(visRow * CELL_H);
  const uint16_t cellH = dh ? CHAR_H * 2u + 1u : CELL_H;

  int16_t idx = ddramIndexAtCell(visCol, visRow);
  const uint8_t* glyph = (idx >= 0) ? glyphFor(ddram[idx]) : nullptr;

  const bool isCursor = (static_cast<int16_t>(visCol) == cursorCol &&
                         static_cast<int16_t>(visRow) == cursorRow);
  const bool blinkOn = isCursor && dispB && blinkPhase;
  const bool underline = isCursor && dispC;

  for (uint16_t cy = 0; cy < cellH; ++cy) {
    uint16_t y = static_cast<uint16_t>(y0 + cy);
    if (y >= h) break;
    uint16_t gr = dh ? (cy >> 1) : cy;  // glyph row (double height: 2x)
    uint8_t bits = 0;
    if (gr < CHAR_H && glyph) bits = glyph[gr] & 0x1Fu;
    if (gr < CHAR_H && blinkOn) bits = 0x1Fu;
    if (gr == CHAR_H - 1u && underline) bits = 0x1Fu;  // OR-lit cursor line
    if (dh && cy >= CHAR_H * 2u) bits = 0;             // trailing gap row
    for (uint16_t cx = 0; cx < CELL_W; ++cx) {
      uint16_t x = static_cast<uint16_t>(x0 + cx);
      if (x >= w) break;
      bool on = (cx < CHAR_W) && ((bits >> (CHAR_W - 1u - cx)) & 1u);
      putPixel(x, y, on);
    }
  }

  if (dirtyTracking) {
    uint16_t x1 = static_cast<uint16_t>(x0 + CELL_W - 1u);
    uint16_t y1 = static_cast<uint16_t>(y0 + cellH - 1u);
    if (x1 >= w) x1 = static_cast<uint16_t>(w - 1u);
    if (y1 >= h) y1 = static_cast<uint16_t>(h - 1u);
    uint16_t px0 = x0, px1 = x1, py0 = y0, py1 = y1;
    const uint8_t flip = static_cast<uint8_t>(config.flipMode);
    if (flip & 0x01u) {
      px0 = static_cast<uint16_t>(w - 1u - x1);
      px1 = static_cast<uint16_t>(w - 1u - x0);
    }
    if (flip & 0x02u) {
      py0 = static_cast<uint16_t>(h - 1u - y1);
      py1 = static_cast<uint16_t>(h - 1u - y0);
    }
    markDirtyRect(py0, py1, px0, px1);
  }
}

void St7032Controller::renderCellForDdramIndex(uint16_t index) {
  uint16_t visCol, visRow;
  if (cellFromDdramIndex(index, &visCol, &visRow)) renderCell(visCol, visRow);
}

// Re-render every visible cell whose character code resolves to the given
// CGRAM character (0..7)
void St7032Controller::renderCellsForCgramChar(uint8_t charIndex) {
  for (uint16_t r = 0; r < config.textRows; ++r) {
    for (uint16_t c = 0; c < config.textCols; ++c) {
      int16_t idx = ddramIndexAtCell(c, r);
      if (idx < 0) continue;
      uint8_t code = ddram[idx];
      if (glyphFor(code) == &cgram[static_cast<uint32_t>(charIndex) * 8u]) {
        renderCell(c, r);
      }
    }
  }
}

void St7032Controller::renderAll() {
  // Recompute the cursor cell first so renderCell() overlays it in place
  uint16_t cc, cr;
  int16_t idx = acInCgram ? -1 : ddramIndex(ac);
  if ((dispC || dispB) && idx >= 0 &&
      cellFromDdramIndex(static_cast<uint16_t>(idx), &cc, &cr)) {
    cursorCol = static_cast<int16_t>(cc);
    cursorRow = static_cast<int16_t>(cr);
  } else {
    cursorCol = -1;
    cursorRow = -1;
  }
  for (uint16_t r = 0; r < config.textRows; ++r) {
    for (uint16_t c = 0; c < config.textCols; ++c) {
      renderCell(c, r);
    }
  }
}

// Recompute the cursor cell from AC and re-render the cells it left/entered
void St7032Controller::updateCursor() {
  int16_t oldCol = cursorCol, oldRow = cursorRow;
  uint16_t cc, cr;
  int16_t idx = acInCgram ? -1 : ddramIndex(ac);
  if ((dispC || dispB) && idx >= 0 &&
      cellFromDdramIndex(static_cast<uint16_t>(idx), &cc, &cr)) {
    cursorCol = static_cast<int16_t>(cc);
    cursorRow = static_cast<int16_t>(cr);
  } else {
    cursorCol = -1;
    cursorRow = -1;
  }
  if (oldCol >= 0 && (oldCol != cursorCol || oldRow != cursorRow)) {
    renderCell(static_cast<uint16_t>(oldCol), static_cast<uint16_t>(oldRow));
  }
  if (cursorCol >= 0) {
    renderCell(static_cast<uint16_t>(cursorCol),
               static_cast<uint16_t>(cursorRow));
  }
}

void St7032Controller::tick(uint32_t nowMs) {
  if (!dispB || writeProtected) return;
  if (nowMs - lastTickMs < BLINK_HALF_PERIOD_MS) return;
  lastTickMs = nowMs;
  blinkPhase = !blinkPhase;
  if (cursorCol >= 0) {
    renderCell(static_cast<uint16_t>(cursorCol),
               static_cast<uint16_t>(cursorRow));
  }
}

}  // namespace lcdtap
