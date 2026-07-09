// Bit-banged I2C master for slave self-testing (diagnostics only).
//
// Timing is deliberately conservative (~50 kHz, half-period delays around
// every edge) so that any reception problem observed with this stimulus
// is attributable to the slave, not the master.

#include "lcdtap/m5tab5/i2c_selftest.hpp"

#include <Arduino.h>

#include <driver/gpio.h>
#include <esp_rom_sys.h>

namespace lcdtap::m5tab5 {

static I2cSelftestConfig sCfg;
static bool sInit = false;

static constexpr uint32_t HALF_US = 10;  // ~50 kHz

// Open-drain emulation: low = drive 0, high = release (pull-up wins).
static inline void sdaSet(bool level) {
  gpio_set_level((gpio_num_t)sCfg.pinSda, level);
}
static inline void sclSet(bool level) {
  gpio_set_level((gpio_num_t)sCfg.pinScl, level);
}
static inline bool sdaGet() { return gpio_get_level((gpio_num_t)sCfg.pinSda); }
static inline void hold() { esp_rom_delay_us(HALF_US); }

static void busStart() {
  // SDA falls while SCL high.
  sdaSet(1);
  sclSet(1);
  hold();
  sdaSet(0);
  hold();
  sclSet(0);
  hold();
}

static void busStop() {
  // SDA rises while SCL high.
  sdaSet(0);
  hold();
  sclSet(1);
  hold();
  sdaSet(1);
  hold();
}

// Send one byte MSB first; returns true when the slave ACKed.
static bool busWriteByte(uint8_t byte) {
  for (int bit = 7; bit >= 0; --bit) {
    sdaSet((byte >> bit) & 1u);
    hold();
    sclSet(1);
    hold();
    sclSet(0);
  }
  // ACK slot: release SDA, sample at SCL high.
  sdaSet(1);
  hold();
  sclSet(1);
  hold();
  bool ack = !sdaGet();
  sclSet(0);
  hold();
  return ack;
}

// Write a transaction; returns the number of ACKed bytes (including the
// address byte).
static int busWrite(const uint8_t *bytes, int len) {
  int acked = 0;
  busStart();
  if (busWriteByte((uint8_t)(sCfg.slaveAddr << 1))) ++acked;
  for (int i = 0; i < len; ++i) {
    if (busWriteByte(bytes[i])) ++acked;
  }
  busStop();
  return acked;
}

void i2cSelftestInit(const I2cSelftestConfig &cfg) {
  sCfg = cfg;
  gpio_config_t io = {};
  io.pin_bit_mask = (1ull << cfg.pinSda) | (1ull << cfg.pinScl);
  io.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&io);
  sdaSet(1);
  sclSet(1);
  sInit = true;
}

void i2cSelftestRun() {
  if (!sInit) return;

  // (a) SSD1306 init sequence as one long write: [ctrl 0x00][commands...]
  static const uint8_t initSeq[] = {0x00, 0xAE, 0xD5, 0x80, 0xA8, 0x3F,
                                    0xD3, 0x00, 0x40, 0x8D, 0x14, 0xA1,
                                    0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xAF};
  int a = busWrite(initSeq, sizeof(initSeq));

  // (b) data write with a recognizable pattern: [ctrl 0x40][0x01,0x02,...]
  uint8_t dataSeq[1 + 32];
  dataSeq[0] = 0x40;
  for (int i = 0; i < 32; ++i) dataSeq[1 + i] = (uint8_t)(i + 1);
  int b = busWrite(dataSeq, sizeof(dataSeq));

  // (c) individual short command writes: [0x00][cmd]
  static const uint8_t shortCmds[] = {0x21, 0x22, 0xA4};
  int c = 0;
  for (uint8_t cmd : shortCmds) {
    const uint8_t seq[2] = {0x00, cmd};
    c += busWrite(seq, 2);
  }

  Serial.printf(
      "[selftest] init: %d/%d acked, data: %d/%d acked, short: %d/%d acked\n",
      a, (int)sizeof(initSeq) + 1, b, (int)sizeof(dataSeq) + 1, c,
      (int)(3 * 3));
}

}  // namespace lcdtap::m5tab5
