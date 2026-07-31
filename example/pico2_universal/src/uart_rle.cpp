#include "uart_rle.hpp"

int rleEncodeSegment(const uint16_t* px, int n, uint8_t* out) {
  int pos = 0;
  int o = 0;
  while (pos < n) {
    // Measure the run starting at pos.
    int run = 1;
    while (pos + run < n && run < RLE_SEG_MAX_PIXELS &&
           px[pos + run] == px[pos]) {
      run++;
    }
    if (run >= 2) {
      out[o++] = static_cast<uint8_t>(0x80u | (run - 1));
      out[o++] = static_cast<uint8_t>(px[pos] & 0xFFu);
      out[o++] = static_cast<uint8_t>(px[pos] >> 8);
      pos += run;
    } else {
      // Literal: extend until a run of >= 2 starts. Never emitting two
      // adjacent literal packets is what bounds the output at 1 + 2 * n.
      int lit = 1;
      while (pos + lit < n && lit < RLE_SEG_MAX_PIXELS &&
             !(pos + lit + 1 < n && px[pos + lit] == px[pos + lit + 1])) {
        lit++;
      }
      out[o++] = static_cast<uint8_t>(lit - 1);
      for (int i = 0; i < lit; i++) {
        out[o++] = static_cast<uint8_t>(px[pos + i] & 0xFFu);
        out[o++] = static_cast<uint8_t>(px[pos + i] >> 8);
      }
      pos += lit;
    }
  }
  return o;
}
