#include "ks0108_controller.hpp"

#include <cstring>

#include <lcdtap/devices/ks0108.hpp>

namespace lcdtap {

uint16_t Ks0108Controller::logicalWidth() const { return config.buffWidth; }

uint16_t Ks0108Controller::logicalHeight() const { return config.buffHeight; }

void Ks0108Controller::updateWriteCache() {
  cachedHOffset = 0;
  cachedHStep = 1;
  cachedVOffset = 0;
  cachedVStep = config.buffWidth;
}

void Ks0108Controller::softReset() {
  for (int chip = 0; chip < NUM_CHIPS; ++chip) {
    chips[chip] = {0, 0, 0, false};
  }
  resetCommon();
  sleeping = false;  // KS0108 has no sleep mode
  setInverted(config.inverted);
}

void Ks0108Controller::dispatchCommand(uint8_t cmd) {
  using namespace ks0108;

  const uint8_t csMask = inputCs & ((1u << NUM_CHIPS) - 1u);
  if (csMask == 0) return;  // no chip selected

  if ((cmd & ~CMD_DISPLAY_ONOFF_MASK) == CMD_DISPLAY_ONOFF_BASE) {
    const bool on = (cmd & CMD_DISPLAY_ONOFF_MASK) != 0u;
    for (int chip = 0; chip < NUM_CHIPS; ++chip) {
      if (csMask & (1u << chip)) chips[chip].on = on;
    }
    // Per-chip blanking cannot be rendered; the display is lit while any
    // chip is on.
    bool anyOn = false;
    for (int chip = 0; chip < NUM_CHIPS; ++chip)
      anyOn = anyOn || chips[chip].on;
    if (anyOn != displayOn) {
      displayOn = anyOn;
      bumpEpoch();
    }
    log(on ? "KS0108: DISPLAY_ON" : "KS0108: DISPLAY_OFF");
    return;
  }

  if ((cmd & ~CMD_SET_COL_MASK) == CMD_SET_COL_BASE) {
    for (int chip = 0; chip < NUM_CHIPS; ++chip) {
      if (csMask & (1u << chip)) chips[chip].col = cmd & CMD_SET_COL_MASK;
    }
    return;
  }

  if ((cmd & ~CMD_SET_PAGE_MASK) == CMD_SET_PAGE_BASE) {
    for (int chip = 0; chip < NUM_CHIPS; ++chip) {
      if (csMask & (1u << chip)) chips[chip].page = cmd & CMD_SET_PAGE_MASK;
    }
    return;
  }

  if ((cmd & ~CMD_SET_START_LINE_MASK) == CMD_SET_START_LINE_BASE) {
    for (int chip = 0; chip < NUM_CHIPS; ++chip) {
      if (csMask & (1u << chip)) {
        setStartLine(chip, cmd & CMD_SET_START_LINE_MASK);
      }
    }
    log("KS0108: SET_START_LINE");
    return;
  }

  noteUnknownCommand(cmd);
}

// Display start line (Z) change: the start line is folded into the
// write-time row mapping, so already-drawn content has to be moved to keep
// the framebuffer consistent — rotate the chip's half so that the row shown
// at the top follows the new Z.
void Ks0108Controller::setStartLine(int chip, uint8_t newZ) {
  ChipState& st = chips[chip];
  const uint8_t oldZ = st.startLine;
  if (oldZ == newZ) return;
  st.startLine = newZ;

  // Screen content shifts up by (newZ - oldZ) rows; in physical rows the
  // direction reverses under vertical flip.
  uint32_t delta = (uint32_t)(newZ - oldZ) & (CHIP_HEIGHT - 1u);
  if (effectiveFlipV()) delta = (CHIP_HEIGHT - delta) & (CHIP_HEIGHT - 1u);
  if (delta == 0) return;

  const uint16_t lcdW = config.buffWidth;
  const uint16_t colBase = chipColBase(chip);
  uint16_t* base = frameBuffer + colBase;

  // Rotate the CHIP_HEIGHT row segments left by delta with three reversals
  // (new[r] = old[(r + delta) % CHIP_HEIGHT]); only one 64px temp row.
  uint16_t tmp[CHIP_WIDTH];
  auto reverseRows = [&](uint32_t a, uint32_t b) {
    while (a < b) {
      uint16_t* pa = base + a * lcdW;
      uint16_t* pb = base + b * lcdW;
      memcpy(tmp, pa, sizeof(tmp));
      memcpy(pa, pb, sizeof(tmp));
      memcpy(pb, tmp, sizeof(tmp));
      ++a;
      --b;
    }
  };
  reverseRows(0, delta - 1u);
  reverseRows(delta, CHIP_HEIGHT - 1u);
  reverseRows(0, CHIP_HEIGHT - 1u);

  markDirtyRect(0, CHIP_HEIGHT - 1u, colBase, colBase + CHIP_WIDTH - 1u);
}

// For KS0108, D/I=1 is always display RAM data — feedDataByte is never called
void Ks0108Controller::feedDataByte(uint8_t /*byte*/) {}

// D/I=1 is always display RAM (pixel) data
bool Ks0108Controller::isRamWriteCommand() const { return true; }

// GRAY1_VPACK8_H2L: 1 byte = 8 vertical pixels (bit0 = top)
void Ks0108Controller::writeDataByte(int chip, uint8_t byte) {
  ChipState& st = chips[chip];
  const uint16_t lcdW = config.buffWidth;

  uint16_t physCol = static_cast<uint16_t>(
      chipColBase(chip) +
      (effectiveFlipH() ? CHIP_WIDTH - 1u - st.col : st.col));
  const uint16_t memRowBase = static_cast<uint16_t>(st.page * 8u);

  uint32_t diff = 0;
  uint16_t physRowMin = 0xFFFFu, physRowMax = 0;
  for (uint16_t bit = 0; bit < 8u; ++bit) {
    // Fold the display start line (scroll) into the row mapping
    uint16_t row =
        (uint16_t)(memRowBase + bit - st.startLine) & (CHIP_HEIGHT - 1u);
    uint16_t physRow =
        effectiveFlipV() ? static_cast<uint16_t>(CHIP_HEIGHT - 1u - row) : row;
    uint16_t px = static_cast<uint16_t>((byte & 1u) ? 0xFFFFu : 0x0000u);
    uint16_t* dst = &frameBuffer[physRow * lcdW + physCol];
    if (dirtyTracking) {
      diff |= static_cast<uint32_t>(*dst ^ px);
      if (physRow < physRowMin) physRowMin = physRow;
      if (physRow > physRowMax) physRowMax = physRow;
    }
    *dst = px;
    byte >>= 1;
  }
  if (diff != 0) {
    markDirtyRect(physRowMin, physRowMax, physCol, physCol);
  }

  // Column auto-increment wraps within the chip (never crosses chips)
  st.col = (st.col + 1u) & (CHIP_WIDTH - 1u);
}

void Ks0108Controller::processRamwrData(const uint8_t* data, uint32_t numBytes,
                                        uint32_t stride) {
  const uint8_t csMask = inputCs & ((1u << NUM_CHIPS) - 1u);
  if (csMask == 0) return;  // no chip selected

  const uint32_t byteLen = numBytes * stride;
  for (uint32_t i = 0; i < byteLen; i += stride) {
    const uint8_t byte = data[i];
    // cs = 3 duplicates the byte to both chips
    for (int chip = 0; chip < NUM_CHIPS; ++chip) {
      if (csMask & (1u << chip)) writeDataByte(chip, byte);
    }
  }
}

}  // namespace lcdtap
