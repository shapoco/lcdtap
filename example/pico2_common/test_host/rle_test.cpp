// Host-side unit test for the getframebuffer RGB565-RLE encoder
// (example/pico2_universal/src/uart_rle.cpp, no MCU dependencies).
//
// Encodes segments of 1..128 pixels across solid, noise, alternating and
// mixed-run patterns, decodes them with an independent reference decoder and
// checks the round trip is lossless. Also pins the worst-case size bound
// (output <= 1 + 2 * n bytes), which is what keeps incompressible images
// from inflating the transfer.
//
// Build & run:
//   g++ -O2 -Wall -Wextra -I../../pico2_universal/include
//       -o /tmp/lcdtap_rle_test rle_test.cpp
//       ../../pico2_universal/src/uart_rle.cpp
//   /tmp/lcdtap_rle_test

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "uart_rle.hpp"

static int gFailures = 0;

// Reference decoder for the packet stream produced by rleEncodeSegment.
static std::vector<uint16_t> rleDecode(const uint8_t* data, int len) {
  std::vector<uint16_t> out;
  int i = 0;
  while (i < len) {
    uint8_t c = data[i++];
    int count = (c & 0x7F) + 1;
    if (c & 0x80) {
      if (i + 2 > len) {
        printf("FAIL: truncated run packet\n");
        gFailures++;
        return out;
      }
      uint16_t px = static_cast<uint16_t>(data[i] | (data[i + 1] << 8));
      i += 2;
      for (int k = 0; k < count; k++) out.push_back(px);
    } else {
      if (i + count * 2 > len) {
        printf("FAIL: truncated literal packet\n");
        gFailures++;
        return out;
      }
      for (int k = 0; k < count; k++) {
        out.push_back(static_cast<uint16_t>(data[i] | (data[i + 1] << 8)));
        i += 2;
      }
    }
  }
  return out;
}

static void checkRoundTrip(const char* name, const uint16_t* px, int n) {
  uint8_t out[RLE_SEG_MAX_BYTES + 16];
  memset(out, 0xA5, sizeof(out));
  int len = rleEncodeSegment(px, n, out);

  if (len > 1 + 2 * n) {
    printf("FAIL: %s n=%d: output %d exceeds bound %d\n", name, n, len,
           1 + 2 * n);
    gFailures++;
  }

  std::vector<uint16_t> dec = rleDecode(out, len);
  if (static_cast<int>(dec.size()) != n) {
    printf("FAIL: %s n=%d: decoded %d pixels\n", name, n,
           static_cast<int>(dec.size()));
    gFailures++;
    return;
  }
  for (int i = 0; i < n; i++) {
    if (dec[i] != px[i]) {
      printf("FAIL: %s n=%d: pixel %d: got 0x%04X expected 0x%04X\n", name, n,
             i, dec[i], px[i]);
      gFailures++;
      return;
    }
  }
}

int main() {
  uint16_t seg[RLE_SEG_MAX_PIXELS];
  srand(12345);

  for (int n = 1; n <= RLE_SEG_MAX_PIXELS; n++) {
    // Solid fill: expect near-maximal compression.
    for (int i = 0; i < n; i++) seg[i] = 0x1234;
    checkRoundTrip("solid", seg, n);

    // Random noise: mostly literals, exercises the size bound.
    for (int i = 0; i < n; i++) seg[i] = static_cast<uint16_t>(rand());
    checkRoundTrip("noise", seg, n);

    // Alternating ABAB: no runs at all, pure literal.
    for (int i = 0; i < n; i++) seg[i] = (i & 1) ? 0xFFFF : 0x0000;
    checkRoundTrip("alternating", seg, n);

    // Short runs mixed with literals (AABCC DDEFF ...).
    for (int i = 0; i < n; i++) {
      int phase = i % 5;
      uint16_t base = static_cast<uint16_t>((i / 5) * 3);
      seg[i] = static_cast<uint16_t>(base + (phase < 2    ? 0
                                             : phase == 2 ? 1
                                                          : 2));
    }
    checkRoundTrip("mixed", seg, n);

    // Random run lengths from a tiny palette (long and short runs).
    {
      int i = 0;
      while (i < n) {
        uint16_t px = static_cast<uint16_t>(rand() & 3);
        int run = 1 + (rand() % 10);
        while (run-- > 0 && i < n) seg[i++] = px;
      }
      checkRoundTrip("palette-runs", seg, n);
    }
  }

  // Solid compression sanity: 128 identical pixels must fit in one packet.
  for (int i = 0; i < RLE_SEG_MAX_PIXELS; i++) seg[i] = 0xBEEF;
  uint8_t out[RLE_SEG_MAX_BYTES];
  int len = rleEncodeSegment(seg, RLE_SEG_MAX_PIXELS, out);
  if (len != 3) {
    printf("FAIL: solid 128px encoded to %d bytes (expected 3)\n", len);
    gFailures++;
  }

  if (gFailures == 0) {
    printf("PASS: all RLE round-trip and size-bound tests\n");
    return 0;
  }
  printf("%d failure(s)\n", gFailures);
  return 1;
}
