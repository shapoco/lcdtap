#include "uart_b64.hpp"

static constexpr char kB64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void b64Encode3(const uint8_t in[3], char out[4]) {
  out[0] = kB64Table[in[0] >> 2];
  out[1] = kB64Table[((in[0] & 0x03u) << 4) | (in[1] >> 4)];
  out[2] = kB64Table[((in[1] & 0x0Fu) << 2) | (in[2] >> 6)];
  out[3] = kB64Table[in[2] & 0x3Fu];
}

void b64EncodePad(const uint8_t* in, int count, char out[4]) {
  uint8_t buf[3] = {0, 0, 0};
  for (int i = 0; i < count; i++) buf[i] = in[i];
  b64Encode3(buf, out);
  if (count == 1) {
    out[2] = '=';
    out[3] = '=';
  } else if (count == 2) {
    out[3] = '=';
  }
}
