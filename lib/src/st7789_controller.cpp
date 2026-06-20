#include "st7789_controller.hpp"

namespace lcdtap {

void St7789Controller::onFeedDataByte(uint8_t byte) {
  using namespace st7789;
  if (currentCmd == CMD_RAMCTRL) {
    if (cmdDataLen ==
        1) {  // P2: bit 3 = ENDIAN (0=big-endian, 1=little-endian)
      cachedLittleEndian = (byte >> 3) & 1u;
    }
    ++cmdDataLen;
  }
}

InterfaceFormat St7789Controller::colmodToFormat(uint8_t fmt) const {
  if (fmt == 0x01u) return InterfaceFormat::RGB111_HPACK2_H2L_RA8;
  if (fmt == 0x03u) return InterfaceFormat::RGB444_HPACK2_H2L_BE;
  return SpiDisplayBase::colmodToFormat(fmt);
}

}  // namespace lcdtap
