#pragma once

#include "controller_base.hpp"

namespace lcdtap {

// ST7032 character LCD controller implementation
//
// Key differences from the pixel-based controllers:
// - RS=0 bytes are single-byte commands (HD44780-style, decoded by highest
//   set bit); RS=1 bytes are always DDRAM/CGRAM data
// - Characters are rendered from CGROM (font_st7032) / CGRAM into the RGB565
//   framebuffer; the framebuffer size is derived from textCols/textRows
//   by normalizeConfig() (cell = 5x8 glyph + 1px gap)
// - In 4-bit bus mode (Function Set DL=0, PARALLEL bus only) two consecutive
//   bytes carry the high/low nibble on D[7:4]
// - Cursor: C bit lights the 8th glyph line (OR, as real hardware); B bit
//   alternates the whole cell between all-on and normal every blink phase,
//   driven by tick()
class St7032Controller : public ControllerBase {
 public:
  static constexpr uint16_t DDRAM_SIZE = 80;
  static constexpr uint16_t CGRAM_SIZE = 64;  // 8 chars x 8 rows
  static constexpr uint16_t CHAR_W = 5;
  static constexpr uint16_t CHAR_H = 8;
  static constexpr uint16_t CELL_W = CHAR_W + 1;
  static constexpr uint16_t CELL_H = CHAR_H + 1;
  static constexpr uint32_t BLINK_HALF_PERIOD_MS = 500;

  uint8_t ddram[DDRAM_SIZE];
  uint8_t cgram[CGRAM_SIZE];

  uint8_t ac;      // address counter (7-bit DDRAM / 6-bit CGRAM)
  bool acInCgram;  // true: AC addresses CGRAM
  uint8_t shift;   // display shift (N=1: 0..39, N=0: 0..79)

  bool entryInc;    // Entry Mode I/D (true: increment)
  bool entryShift;  // Entry Mode S (true: shift display on data write)
  bool dispC;       // cursor on
  bool dispB;       // cursor blink on
  bool funcDL;      // 8-bit bus (false: 4-bit nibble mode on PARALLEL)
  bool funcN;       // 2-line mode
  bool funcDH;      // double-height font (N=0 only)
  bool funcIS;      // instruction table select (table 1 is ignored)

  // 4-bit mode nibble pairing (high nibble of each byte, high-first)
  bool nibblePending;
  uint8_t nibbleHigh;
  bool nibbleIsData;  // RS of the pending nibble; mismatch resyncs

  // Cursor/blink rendering state
  bool blinkPhase;               // true: blink cell currently forced all-on
  uint32_t lastTickMs;           // timestamp of the last blink phase flip
  int16_t cursorCol, cursorRow;  // last rendered cursor cell (-1: none)

  uint16_t logicalWidth() const override;
  uint16_t logicalHeight() const override;
  void updateWriteCache() override;
  void softReset() override;
  void dispatchCommand(uint8_t cmd) override;
  void feedDataByte(uint8_t byte) override;
  bool isRamWriteCommand() const override;
  void processRamwrData(const uint8_t* data, uint32_t numBytes,
                        uint32_t stride) override;
  void tick(uint32_t nowMs) override;

 private:
  bool nibbleMode() const;
  void execCommand(uint8_t cmd);
  void writeData(uint8_t byte);
  void advanceAc(bool inc);

  // ddram[] index for a DDRAM address, or -1 if the address is unmapped
  int16_t ddramIndex(uint8_t addr) const;

  // Visible cell (visCol, visRow) for a ddram[] index, or false if hidden
  bool cellFromDdramIndex(uint16_t index, uint16_t* visCol,
                          uint16_t* visRow) const;

  // ddram[] index shown at a visible cell, or -1 if the cell is blank
  int16_t ddramIndexAtCell(uint16_t visCol, uint16_t visRow) const;

  const uint8_t* glyphFor(uint8_t code) const;
  void putPixel(uint16_t x, uint16_t y, bool on);
  void renderCell(uint16_t visCol, uint16_t visRow);
  void renderCellForDdramIndex(uint16_t index);
  void renderCellsForCgramChar(uint8_t charIndex);
  void renderAll();
  void updateCursor();
};

}  // namespace lcdtap
