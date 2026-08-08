#include "testrig/vectors.hpp"

namespace testrig {

using lcdtap::BusType;
using lcdtap::ConfigPreset;
using lcdtap::ControllerFamily;
using lcdtap::InterfaceFormat;
using lcdtap::TrimMode;

namespace {

constexpr uint32_t I2C_FAST = 1000000;   // 100k / 400k / 1M / 2M
constexpr uint32_t SPI_FAST = 40000000;  // 10M / 20M / 40M / 60M
constexpr uint32_t PAR_FAST = 5000000;   // 1M / 2.5M / 5M / 10M
// Character LCDs on the parallel bus: real HD44780-class modules (e.g.
// SC2004) need ~100 us per byte, so the clock stays in the kHz range.
constexpr uint32_t PAR_TEXT_FAST = 10000;  // 2k / 5k / 10k / 20k

constexpr InterfaceFormat FMT_NONE = InterfaceFormat::NUM_FORMATS;

// Trim rectangle rule: x=11, y=15, w=buffWidth/2, h=buffHeight/2.
constexpr uint16_t TRIM_X = 11;
constexpr uint16_t TRIM_Y = 15;

}  // namespace

// clang-format off
const TestVector TEST_VECTORS[] = {
    // name              preset                     bus                 freq      WxH        format                                 rot trim           tx      ty      tw   th
    {"SSD1306_I2C",    ConfigPreset::SSD1306,   BusType::I2C,       I2C_FAST, 128,  64, InterfaceFormat::GRAY1_VPACK8_H2L,      0, TrimMode::OFF,    0, 0, 0, 0},
    {"SSD1306_Rot1",   ConfigPreset::SSD1306,   BusType::I2C,       I2C_FAST, 128,  64, InterfaceFormat::GRAY1_VPACK8_H2L,      1, TrimMode::OFF,    0, 0, 0, 0},
    {"SSD1306_Rot2",   ConfigPreset::SSD1306,   BusType::I2C,       I2C_FAST, 128,  64, InterfaceFormat::GRAY1_VPACK8_H2L,      2, TrimMode::OFF,    0, 0, 0, 0},
    {"SSD1306_Rot3",   ConfigPreset::SSD1306,   BusType::I2C,       I2C_FAST, 128,  64, InterfaceFormat::GRAY1_VPACK8_H2L,      3, TrimMode::OFF,    0, 0, 0, 0},
    {"SSD1306_SPI",    ConfigPreset::SSD1306,   BusType::SPI_4LINE, SPI_FAST, 128,  64, InterfaceFormat::GRAY1_VPACK8_H2L,      2, TrimMode::OFF,    0, 0, 0, 0},
    {"SSD1331_RGB332", ConfigPreset::SSD1331,   BusType::SPI_4LINE, SPI_FAST,  96,  64, InterfaceFormat::RGB332,                0, TrimMode::OFF,    0, 0, 0, 0},
    {"SSD1331_Rot1",   ConfigPreset::SSD1331,   BusType::SPI_4LINE, SPI_FAST,  96,  64, InterfaceFormat::RGB332,                1, TrimMode::OFF,    0, 0, 0, 0},
    {"SSD1331_Rot2",   ConfigPreset::SSD1331,   BusType::SPI_4LINE, SPI_FAST,  96,  64, InterfaceFormat::RGB332,                2, TrimMode::OFF,    0, 0, 0, 0},
    {"SSD1331_Rot3",   ConfigPreset::SSD1331,   BusType::SPI_4LINE, SPI_FAST,  96,  64, InterfaceFormat::RGB332,                3, TrimMode::OFF,    0, 0, 0, 0},
    {"SSD1331_RGB565", ConfigPreset::SSD1331,   BusType::SPI_4LINE, SPI_FAST,  96,  64, InterfaceFormat::RGB565_BE,             2, TrimMode::OFF,    0, 0, 0, 0},
    {"SSD1331_RGB666", ConfigPreset::SSD1331,   BusType::SPI_4LINE, SPI_FAST,  96,  64, InterfaceFormat::RGB666_UNPACK_RA8_BE,  2, TrimMode::OFF,    0, 0, 0, 0},
    {"ST7789_RGB444",  ConfigPreset::ST7789,    BusType::SPI_4LINE, SPI_FAST, 240, 320, InterfaceFormat::RGB444_HPACK2_H2L_BE,  0, TrimMode::OFF,    0, 0, 0, 0},
    {"ST7789_Rot1",    ConfigPreset::ST7789,    BusType::SPI_4LINE, SPI_FAST, 240, 320, InterfaceFormat::RGB444_HPACK2_H2L_BE,  1, TrimMode::OFF,    0, 0, 0, 0},
    {"ST7789_Rot2",    ConfigPreset::ST7789,    BusType::SPI_4LINE, SPI_FAST, 240, 320, InterfaceFormat::RGB444_HPACK2_H2L_BE,  2, TrimMode::OFF,    0, 0, 0, 0},
    {"ST7789_Rot3",    ConfigPreset::ST7789,    BusType::SPI_4LINE, SPI_FAST, 240, 320, InterfaceFormat::RGB444_HPACK2_H2L_BE,  3, TrimMode::OFF,    0, 0, 0, 0},
    {"ST7789_RGB565",  ConfigPreset::ST7789,    BusType::SPI_4LINE, SPI_FAST, 240, 320, InterfaceFormat::RGB565_BE,             3, TrimMode::OFF,    0, 0, 0, 0},
    {"ST7789_RGB666",  ConfigPreset::ST7789,    BusType::SPI_4LINE, SPI_FAST, 240, 320, InterfaceFormat::RGB666_UNPACK_LA8_BE,  3, TrimMode::OFF,    0, 0, 0, 0},
    {"ILI9341_SPI",    ConfigPreset::ILI9341,   BusType::SPI_4LINE, SPI_FAST, 240, 320, InterfaceFormat::RGB565_BE,             3, TrimMode::OFF,    0, 0, 0, 0},
    {"ILI9488_RGB111", ConfigPreset::ILI9488,   BusType::SPI_4LINE, SPI_FAST, 320, 480, InterfaceFormat::RGB111_HPACK2_H2L_RA8, 0, TrimMode::OFF,    0, 0, 0, 0},
    {"ILI9488_Rot1",   ConfigPreset::ILI9488,   BusType::SPI_4LINE, SPI_FAST, 320, 480, InterfaceFormat::RGB111_HPACK2_H2L_RA8, 1, TrimMode::OFF,    0, 0, 0, 0},
    {"ILI9488_Rot2",   ConfigPreset::ILI9488,   BusType::SPI_4LINE, SPI_FAST, 320, 480, InterfaceFormat::RGB111_HPACK2_H2L_RA8, 2, TrimMode::OFF,    0, 0, 0, 0},
    {"ILI9488_Rot3",   ConfigPreset::ILI9488,   BusType::SPI_4LINE, SPI_FAST, 320, 480, InterfaceFormat::RGB111_HPACK2_H2L_RA8, 3, TrimMode::OFF,    0, 0, 0, 0},
    {"ILI9488_RGB565", ConfigPreset::ILI9488,   BusType::SPI_4LINE, SPI_FAST, 320, 480, InterfaceFormat::RGB565_BE,             3, TrimMode::OFF,    0, 0, 0, 0},
    {"ILI9488_RGB666", ConfigPreset::ILI9488,   BusType::SPI_4LINE, SPI_FAST, 320, 480, InterfaceFormat::RGB666_UNPACK_LA8_BE,  3, TrimMode::OFF,    0, 0, 0, 0},
    {"ILI9488_TrimC",  ConfigPreset::ILI9488,   BusType::SPI_4LINE, SPI_FAST, 320, 480, InterfaceFormat::RGB565_BE,             3, TrimMode::CUSTOM, TRIM_X, TRIM_Y, 160, 240},
    {"ILI9488_TrimA",  ConfigPreset::ILI9488,   BusType::SPI_4LINE, SPI_FAST, 320, 480, InterfaceFormat::RGB565_BE,             3, TrimMode::AUTO,   TRIM_X, TRIM_Y, 160, 240},
    {"ILI9488_Par8",   ConfigPreset::ILI9488,   BusType::PARALLEL,  PAR_FAST, 320, 480, InterfaceFormat::RGB565_BE,             3, TrimMode::OFF,    0, 0, 0, 0},
    {"TEXT_8x2",       ConfigPreset::TEXT_0802, BusType::I2C,       I2C_FAST,   0,   0, FMT_NONE,                               0, TrimMode::OFF,    0, 0, 0, 0},
    {"TEXT_16x2",      ConfigPreset::TEXT_1602, BusType::I2C,       I2C_FAST,   0,   0, FMT_NONE,                               0, TrimMode::OFF,    0, 0, 0, 0},
    {"TEXT_16x4",      ConfigPreset::TEXT_1604, BusType::I2C,       I2C_FAST,   0,   0, FMT_NONE,                               0, TrimMode::OFF,    0, 0, 0, 0},
    {"TEXT_20x4",      ConfigPreset::TEXT_2004, BusType::I2C,       I2C_FAST,   0,   0, FMT_NONE,                               0, TrimMode::OFF,    0, 0, 0, 0},
    {"TEXT_Par8",      ConfigPreset::TEXT_2004, BusType::PARALLEL,  PAR_TEXT_FAST, 0, 0, FMT_NONE,                             0, TrimMode::OFF,    0, 0, 0, 0},
};
// clang-format on

const int NUM_TEST_VECTORS =
    static_cast<int>(sizeof(TEST_VECTORS) / sizeof(TEST_VECTORS[0]));

bool vectorIsText(const TestVector& v) { return v.interfaceFormat == FMT_NONE; }

lcdtap::ControllerFamily presetFamily(ConfigPreset preset) {
  switch (preset) {
    case ConfigPreset::SSD1306: return ControllerFamily::SSD1306;
    case ConfigPreset::SSD1331: return ControllerFamily::SSD1331;
    case ConfigPreset::ILI9341:
    case ConfigPreset::ILI9342:
    case ConfigPreset::ILI9488: return ControllerFamily::ILI9341;
    case ConfigPreset::TEXT_0802:
    case ConfigPreset::TEXT_1602:
    case ConfigPreset::TEXT_1604:
    case ConfigPreset::TEXT_2004: return ControllerFamily::ST7032;
    default: return ControllerFamily::ST7789;
  }
}

int allowedBuses(ControllerFamily fam, BusType* out, int cap) {
  int n = 0;
  auto add = [&](BusType b) {
    if (n < cap) out[n++] = b;
  };
  switch (fam) {
    case ControllerFamily::SSD1306:
    case ControllerFamily::SSD1331:
      add(BusType::I2C);
      add(BusType::SPI_4LINE);
      break;
    case ControllerFamily::ST7032:
      add(BusType::I2C);
      add(BusType::PARALLEL);
      break;
    default:  // ST7789 / ILI9341
      add(BusType::SPI_4LINE);
      add(BusType::PARALLEL);
      break;
  }
  return n;
}

int freqChoices(ControllerFamily fam, BusType bus, uint32_t* out, int cap) {
  static const uint32_t I2C_FREQS[] = {100000, 400000, 1000000, 2000000};
  static const uint32_t SPI_FREQS[] = {10000000, 20000000, 40000000, 60000000};
  static const uint32_t PAR_FREQS[] = {1000000, 2500000, 5000000, 10000000};
  static const uint32_t PAR_TEXT_FREQS[] = {2000, 5000, 10000, 20000};
  const uint32_t* src;
  switch (bus) {
    case BusType::I2C: src = I2C_FREQS; break;
    case BusType::PARALLEL:
      src = (fam == ControllerFamily::ST7032) ? PAR_TEXT_FREQS : PAR_FREQS;
      break;
    default: src = SPI_FREQS; break;
  }
  int n = 0;
  for (; n < 4 && n < cap; n++) out[n] = src[n];
  return n;
}

uint32_t defaultFreq(ControllerFamily fam, BusType bus) {
  uint32_t f[4];
  freqChoices(fam, bus, f, 4);
  return f[2];  // "Fast"
}

int resolutionChoices(ControllerFamily fam, uint16_t* w, uint16_t* h, int cap) {
  int n = 0;
  auto add = [&](uint16_t ww, uint16_t hh) {
    if (n < cap) {
      w[n] = ww;
      h[n] = hh;
      n++;
    }
  };
  switch (fam) {
    case ControllerFamily::SSD1306:
      add(128, 64);
      add(128, 32);
      break;
    case ControllerFamily::SSD1331: add(96, 64); break;
    case ControllerFamily::ST7032: break;  // derived from textCols/textRows
    default:                               // ST7789 / ILI9341
      add(128, 160);
      add(240, 240);
      add(240, 320);
      add(320, 480);
      break;
  }
  return n;
}

int formatChoices(ControllerFamily fam, InterfaceFormat* out, int cap) {
  int n = 0;
  auto add = [&](InterfaceFormat f) {
    if (n < cap) out[n++] = f;
  };
  switch (fam) {
    case ControllerFamily::SSD1306:
      add(InterfaceFormat::GRAY1_VPACK8_H2L);
      break;
    case ControllerFamily::SSD1331:
      add(InterfaceFormat::RGB332);
      add(InterfaceFormat::RGB565_BE);
      add(InterfaceFormat::RGB666_UNPACK_RA8_BE);
      break;
    case ControllerFamily::ST7789:
      add(InterfaceFormat::RGB444_HPACK2_H2L_BE);
      add(InterfaceFormat::RGB565_BE);
      add(InterfaceFormat::RGB666_UNPACK_LA8_BE);
      break;
    case ControllerFamily::ILI9341:
      add(InterfaceFormat::RGB111_HPACK2_H2L_RA8);
      add(InterfaceFormat::RGB565_BE);
      add(InterfaceFormat::RGB666_UNPACK_LA8_BE);
      break;
    default: break;
  }
  return n;
}

}  // namespace testrig
