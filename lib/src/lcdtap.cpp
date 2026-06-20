#include <lcdtap/lcdtap.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "controller_base.hpp"
#include "ili9341_controller.hpp"
#include "ssd1306_controller.hpp"
#include "ssd1331_controller.hpp"
#include "st7789_controller.hpp"

namespace lcdtap {

//=============================================================================
// getDefaultConfig
//=============================================================================
DumpConfig getDefaultDumpConfig() { return {}; }

void getDefaultConfig(ControllerFamily type, LcdTapConfig* cfg) {
  *cfg = {};
  memset(cfg, 0, sizeof(*cfg));
  cfg->controllerFamily = type;
  cfg->forcePowerOn = false;
  cfg->interfaceFormatOverride = -1;
  cfg->dviWidth = 640;
  cfg->dviHeight = 480;
  cfg->scaleMode = ScaleMode::FIT;
  cfg->inverted = false;
  cfg->swapRB = false;
  switch (type) {
    case ControllerFamily::ST7789:
      cfg->buffWidth = 240;
      cfg->buffHeight = 320;
      cfg->inverted = true;
      cfg->outputRotation = 0;
      cfg->busInterface = BusType::SPI_4LINE;
      break;
    case ControllerFamily::SSD1306:
      cfg->buffWidth = 128;
      cfg->buffHeight = 64;
      cfg->outputRotation = 2;
      cfg->busInterface = BusType::I2C;
      break;
    case ControllerFamily::SSD1331:
      cfg->buffWidth = 96;
      cfg->buffHeight = 64;
      cfg->outputRotation = 2;
      cfg->busInterface = BusType::SPI_4LINE;
      break;
    case ControllerFamily::ILI9341:
      cfg->buffWidth = 240;
      cfg->buffHeight = 320;
      cfg->inverted = false;
      cfg->outputRotation = 0;
      cfg->busInterface = BusType::SPI_4LINE;
      break;
  }
  cfg->trimMode = TrimMode::OFF;
  cfg->trimX = 0;
  cfg->trimY = 0;
  cfg->trimWidth = cfg->buffWidth;
  cfg->trimHeight = cfg->buffHeight;
}

void getPresetConfig(ConfigPreset preset, LcdTapConfig* cfg) {
  switch (preset) {
    case ConfigPreset::ILI9342:
      getDefaultConfig(ControllerFamily::ST7789, cfg);
      cfg->buffWidth = 320;
      cfg->buffHeight = 240;
      cfg->inverted = true;
      cfg->swapRB = true;
      break;

    case ConfigPreset::ILI9488:
      getDefaultConfig(ControllerFamily::ST7789, cfg);
      cfg->buffWidth = 320;
      cfg->buffHeight = 480;
      cfg->inverted = true;
      break;

    case ConfigPreset::SSD1306:
      getDefaultConfig(ControllerFamily::SSD1306, cfg);
      break;

    case ConfigPreset::SSD1331:
      getDefaultConfig(ControllerFamily::SSD1331, cfg);
      break;

    case ConfigPreset::ST7735:
      getDefaultConfig(ControllerFamily::ST7789, cfg);
      cfg->buffWidth = 128;
      cfg->buffHeight = 160;
      break;

    case ConfigPreset::ST7789:
      getDefaultConfig(ControllerFamily::ST7789, cfg);
      break;

    case ConfigPreset::ARDUBOY:
      getDefaultConfig(ControllerFamily::SSD1306, cfg);
      cfg->busInterface = BusType::SPI_4LINE;
      break;

    case ConfigPreset::M5STACK_CORES3:
      getDefaultConfig(ControllerFamily::ST7789, cfg);
      cfg->buffWidth = 320;
      cfg->buffHeight = 240;
      cfg->inverted = true;
      cfg->swapRB = true;
      cfg->outputRotation = 0;
      break;

    case ConfigPreset::THUMBY:
      getDefaultConfig(ControllerFamily::SSD1306, cfg);
      cfg->busInterface = BusType::SPI_4LINE;
      cfg->trimMode = TrimMode::CUSTOM;
      cfg->trimX = 28;
      cfg->trimY = 24;
      cfg->trimWidth = 72;
      cfg->trimHeight = 40;
      break;

    case ConfigPreset::TINYJOYPAD:
      getDefaultConfig(ControllerFamily::SSD1306, cfg);
      cfg->busInterface = BusType::I2C;
      break;

    case ConfigPreset::XIAMOCON:
      getDefaultConfig(ControllerFamily::ST7789, cfg);
      cfg->buffWidth = 240;
      cfg->buffHeight = 240;
      break;

    default: break;
  }

  if (cfg->trimMode != TrimMode::CUSTOM) {
    cfg->trimX = 0;
    cfg->trimY = 0;
    cfg->trimWidth = cfg->buffWidth;
    cfg->trimHeight = cfg->buffHeight;
  }
}

InterfaceFormat getDefaultInterfaceFormat(ControllerFamily type) {
  switch (type) {
    case ControllerFamily::ST7789: return InterfaceFormat::RGB565_BE;
    case ControllerFamily::SSD1306: return InterfaceFormat::GRAY1_VPACK8_H2L;
    case ControllerFamily::SSD1331: return InterfaceFormat::RGB332;
    case ControllerFamily::ILI9341: return InterfaceFormat::RGB565_BE;
    default: return InterfaceFormat::RGB565_BE;
  }
}

const char* getShortInterfaceFormatName(InterfaceFormat fmt) {
  switch (fmt) {
    case InterfaceFormat::GRAY1_VPACK8_H2L: return "GRAY1";
    case InterfaceFormat::RGB111_HPACK2_H2L_RA8: return "RGB111";
    case InterfaceFormat::RGB332: return "RGB332";
    case InterfaceFormat::RGB444_HPACK2_H2L_BE: return "RGB444";
    case InterfaceFormat::RGB565_BE: return "RGB565";
    case InterfaceFormat::RGB666_UNPACK_LA8_BE: return "RGB666_LA";
    case InterfaceFormat::RGB666_UNPACK_RA8_BE: return "RGB666_RA";
    default: return "(undefined)";
  }
}

void getConfigEntryById(ConfigId id, ConfigEntry* e) {
  memset(e, 0, sizeof(*e));
  e->unit = "";
  e->step = 1;
  e->max = 1;
  e->enableKeyId = -1;

  switch (id) {
    // Controller Family
    case ConfigId::CTRL_FAMILY:
      e->type = ValueType::ENUM;
      e->name = "Controller Family";
      e->options = CONTROLLER_NAMES;
      e->max = static_cast<int16_t>(ControllerFamily::NUM_CONTROLLERS) - 1;
      break;

    // Bus Interface
    case ConfigId::BUS_INTERFACE:
      e->type = ValueType::ENUM;
      e->name = "Bus Interface";
      e->options = INTERFACE_NAMES;
      e->max = static_cast<int16_t>(BusType::NUM_INTERFACES) - 1;
      break;

    // Frame Buffer Width
    case ConfigId::BUFF_WIDTH:
      e->type = ValueType::INT16;
      e->name = "Buffer Width";
      e->unit = "px";
      e->options = nullptr;
      e->min = 32;
      e->max = 480;
      e->step = 8;
      break;

    // Frame Buffer Height
    case ConfigId::BUFF_HEIGHT:
      e->type = ValueType::INT16;
      e->name = "Buffer Height";
      e->unit = "px";
      e->options = nullptr;
      e->min = 32;
      e->max = 480;
      e->step = 8;
      break;

    // Inversion
    case ConfigId::INVERSION:
      e->type = ValueType::BOOL;
      e->name = "Inversion";
      e->options = ON_OFF_NAMES;
      break;

    // Swap Red/Blue
    case ConfigId::SWAP_RB:
      e->type = ValueType::BOOL;
      e->name = "Swap Red/Blue";
      e->options = ON_OFF_NAMES;

      // Enable for all color controllers (no-op for SSD1306).
      e->enableKeyId = static_cast<int16_t>(ConfigId::CTRL_FAMILY);
      e->enableKeyValueMin = static_cast<int16_t>(ControllerFamily::ST7789);
      e->enableKeyValueMax =
          static_cast<int16_t>(ControllerFamily::NUM_CONTROLLERS) - 1;
      break;

    // Force Power On
    case ConfigId::FORCE_PWR_ON:
      e->type = ValueType::BOOL;
      e->name = "Force Power On";
      e->options = ON_OFF_NAMES;
      break;

    // Format Override
    case ConfigId::INTF_FMT_OVR:
      e->type = ValueType::ENUM;
      e->name = "Format Override";
      e->options = INTERFACE_FORMAT_NAMES;
      e->min = -1;
      e->max = static_cast<int16_t>(InterfaceFormat::NUM_FORMATS) - 1;

      // Enable for all color controllers (no-op for SSD1306).
      e->enableKeyId = static_cast<int16_t>(ConfigId::CTRL_FAMILY);
      e->enableKeyValueMin = static_cast<int16_t>(ControllerFamily::ST7789);
      e->enableKeyValueMax =
          static_cast<int16_t>(ControllerFamily::NUM_CONTROLLERS) - 1;
      break;

    // Trimming Mode
    case ConfigId::TRIM_MODE:
      e->type = ValueType::ENUM;
      e->name = "Trimming Mode";
      e->options = TRIM_MODE_NAMES;
      e->max = static_cast<int16_t>(TrimMode::NUM_MODES) - 1;
      break;

    // Offset X
    case ConfigId::TRIM_X:
      e->type = ValueType::INT16;
      e->name = "Trim Offset X";
      e->unit = "px";
      e->max = 480;

      e->enableKeyId = static_cast<int16_t>(ConfigId::TRIM_MODE);
      e->enableKeyValueMin = static_cast<int16_t>(TrimMode::CUSTOM);
      e->enableKeyValueMax = static_cast<int16_t>(TrimMode::CUSTOM);
      break;

    // Offset Y
    case ConfigId::TRIM_Y:
      e->type = ValueType::INT16;
      e->name = "Trim Offset Y";
      e->unit = "px";
      e->max = 480;

      e->enableKeyId = static_cast<int16_t>(ConfigId::TRIM_MODE);
      e->enableKeyValueMin = static_cast<int16_t>(TrimMode::CUSTOM);
      e->enableKeyValueMax = static_cast<int16_t>(TrimMode::CUSTOM);
      break;

    // Trim Width
    case ConfigId::TRIM_WIDTH:
      e->type = ValueType::INT16;
      e->name = "Trim Width";
      e->unit = "px";
      e->max = 480;

      e->enableKeyId = static_cast<int16_t>(ConfigId::TRIM_MODE);
      e->enableKeyValueMin = static_cast<int16_t>(TrimMode::CUSTOM);
      e->enableKeyValueMax = static_cast<int16_t>(TrimMode::CUSTOM);
      break;

    // Trim Height
    case ConfigId::TRIM_HEIGHT:
      e->type = ValueType::INT16;
      e->name = "Trim Height";
      e->unit = "px";
      e->max = 480;

      e->enableKeyId = static_cast<int16_t>(ConfigId::TRIM_MODE);
      e->enableKeyValueMin = static_cast<int16_t>(TrimMode::CUSTOM);
      e->enableKeyValueMax = static_cast<int16_t>(TrimMode::CUSTOM);
      break;

    // Output Rotation
    case ConfigId::OUTPUT_ROT:
      e->type = ValueType::ENUM;
      e->name = "Output Rotation";
      e->unit = "deg";
      e->options = ROTATION_NAMES;
      e->max = 3;
      break;

    // Output Scaling
    case ConfigId::SCALE_MODE:
      e->type = ValueType::ENUM;
      e->name = "Output Scaling";
      e->options = SCALE_MODE_NAMES;
      e->max = static_cast<int16_t>(ScaleMode::NUM_MODES) - 1;
      break;
    default: break;
  }
}

int16_t getConfigValueById(const LcdTapConfig& cfg, ConfigId id) {
  switch (id) {
    case ConfigId::CTRL_FAMILY:
      return static_cast<int16_t>(cfg.controllerFamily);
    case ConfigId::BUS_INTERFACE: return static_cast<int16_t>(cfg.busInterface);
    case ConfigId::BUFF_WIDTH: return static_cast<int16_t>(cfg.buffWidth);
    case ConfigId::BUFF_HEIGHT: return static_cast<int16_t>(cfg.buffHeight);
    case ConfigId::INVERSION: return cfg.inverted ? 1 : 0;
    case ConfigId::SWAP_RB: return cfg.swapRB ? 1 : 0;
    case ConfigId::FORCE_PWR_ON: return cfg.forcePowerOn ? 1 : 0;
    case ConfigId::INTF_FMT_OVR:
      return static_cast<int16_t>(cfg.interfaceFormatOverride);
    case ConfigId::TRIM_MODE: return static_cast<int16_t>(cfg.trimMode);
    case ConfigId::TRIM_X: return static_cast<int16_t>(cfg.trimX);
    case ConfigId::TRIM_Y: return static_cast<int16_t>(cfg.trimY);
    case ConfigId::TRIM_WIDTH: return static_cast<int16_t>(cfg.trimWidth);
    case ConfigId::TRIM_HEIGHT: return static_cast<int16_t>(cfg.trimHeight);
    case ConfigId::OUTPUT_ROT: return static_cast<int16_t>(cfg.outputRotation);
    case ConfigId::SCALE_MODE: return static_cast<int16_t>(cfg.scaleMode);
    default: return 0;
  }
}

void setConfigValueById(LcdTapConfig* cfg, ConfigId id, int16_t value) {
  switch (id) {
    case ConfigId::CTRL_FAMILY:
      cfg->controllerFamily = static_cast<ControllerFamily>(value);
      break;
    case ConfigId::BUS_INTERFACE:
      cfg->busInterface = static_cast<BusType>(value);
      break;
    case ConfigId::BUFF_WIDTH:
      cfg->buffWidth = static_cast<uint16_t>(value);
      break;
    case ConfigId::BUFF_HEIGHT:
      cfg->buffHeight = static_cast<uint16_t>(value);
      break;
    case ConfigId::INVERSION: cfg->inverted = (value != 0); break;
    case ConfigId::SWAP_RB: cfg->swapRB = (value != 0); break;
    case ConfigId::FORCE_PWR_ON: cfg->forcePowerOn = (value != 0); break;
    case ConfigId::INTF_FMT_OVR:
      cfg->interfaceFormatOverride = static_cast<int8_t>(value);
      break;
    case ConfigId::TRIM_MODE:
      cfg->trimMode = static_cast<TrimMode>(value);
      break;
    case ConfigId::TRIM_X: cfg->trimX = static_cast<uint16_t>(value); break;
    case ConfigId::TRIM_Y: cfg->trimY = static_cast<uint16_t>(value); break;
    case ConfigId::TRIM_WIDTH:
      cfg->trimWidth = static_cast<uint16_t>(value);
      break;
    case ConfigId::TRIM_HEIGHT:
      cfg->trimHeight = static_cast<uint16_t>(value);
      break;
    case ConfigId::OUTPUT_ROT:
      cfg->outputRotation = static_cast<uint8_t>(value & 3u);
      break;
    case ConfigId::SCALE_MODE:
      cfg->scaleMode = static_cast<ScaleMode>(value);
      break;
    default: break;
  }
}

void formatConfigValue(char* buf, int bufLen, const ConfigEntry& item) {
  switch (item.type) {
    case ValueType::BOOL:
    case ValueType::ENUM:
      if (item.options && item.value >= item.min && item.value <= item.max) {
        snprintf(buf, static_cast<size_t>(bufLen), "%s",
                 item.options[item.value - item.min]);
      } else {
        snprintf(buf, static_cast<size_t>(bufLen), "---");
      }
      break;
    case ValueType::INT16:
      snprintf(buf, static_cast<size_t>(bufLen), "%d",
               static_cast<int>(item.value));
      break;
    default: buf[0] = '\0'; break;
  }
}

//=============================================================================
// ControllerBase common implementation
//=============================================================================

void ControllerBase::calcScaleParams() {
  uint16_t videoW = config.dviWidth;
  uint16_t videoH = config.dviHeight;

  uint16_t buffW = config.buffWidth;
  uint16_t buffH = config.buffHeight;

  int16_t trimW = buffW;
  int16_t trimH = buffH;
  if (config.trimMode != TrimMode::OFF) {
    trimW = config.trimWidth;
    trimH = config.trimHeight;
  }

  int16_t trimX = 0;
  int16_t trimY = 0;
  if (config.trimMode != TrimMode::OFF) {
    trimX = config.trimX;
    trimY = config.trimY;
  }

  // Trim area must be within the buffer area
  int16_t srcW = trimW;
  int16_t srcH = trimH;
  if (trimX < 0) {
    srcW += trimX;
    trimX = 0;
  } else if (trimX + srcW > buffW) {
    srcW = buffW - trimX;
  }
  if (trimY < 0) {
    srcH += trimY;
    trimY = 0;
  } else if (trimY + srcH > buffH) {
    srcH = buffH - trimY;
  }

  if (srcW <= 0 || srcH <= 0) {
    // No valid trim area; disable scaling and output
    outDestX = 0;
    outDestY = 0;
    outDestW = 0;
    outDestH = 0;
    outSrcX = 0;
    outSrcY = 0;
    outSrcW = 0;
    outSrcH = 0;
    outSrcStepH = 0;
    outSrcStepV = 0;
    return;
  }

  // rot=1,3: LCD width and height appear swapped
  if ((config.outputRotation & 1) != 0) {
    std::swap(buffW, buffH);
    std::swap(srcW, srcH);
  }

  switch (config.scaleMode) {
    case ScaleMode::STRETCH:
      // Fill the entire video area (ignore aspect ratio)
      outDestX = 0;
      outDestY = 0;
      outDestW = videoW;
      outDestH = videoH;
      break;

    case ScaleMode::FIT:
      // Scale to fit the video area while preserving aspect ratio; add black
      // padding if needed
      if ((uint32_t)srcW * videoH > (uint32_t)videoW * srcH) {
        outDestW = videoW;
        outDestH = (uint16_t)((uint32_t)videoW * srcH / srcW);
      } else {
        outDestH = videoH;
        outDestW = (uint16_t)((uint32_t)videoH * srcW / srcH);
      }
      outDestX = (videoW - outDestW) / 2;
      outDestY = (videoH - outDestH) / 2;
      break;

    case ScaleMode::PIXEL_PERFECT: {
      // Scale by the largest integer factor that fits in the video area; add
      // black padding if needed
      uint16_t scaleH = videoW / srcW;
      uint16_t scaleV = videoH / srcH;
      uint16_t scale = scaleH < scaleV ? scaleH : scaleV;
      if (scale == 0) scale = 1;
      outDestW = srcW * scale;
      outDestH = srcH * scale;
      outDestX = (videoW - outDestW) / 2;
      outDestY = (videoH - outDestH) / 2;
      break;
    }
  }

  outSrcX = static_cast<uint16_t>(trimX);
  outSrcY = static_cast<uint16_t>(trimY);
  outSrcW = static_cast<uint16_t>(trimW);
  outSrcH = static_cast<uint16_t>(trimH);
  outSrcStepH = ((uint32_t)srcW << 16) / outDestW;
  outSrcStepV = ((uint32_t)srcH << 16) / outDestH;
}

void ControllerBase::log(const char* msg) const {
  if (host.log) host.log(host.userData, msg);
}

void ControllerBase::resetCommon() {
  sleeping = true;
  displayOn = false;
  inverted = false;
  cachedLittleEndian = false;
  interfaceFormat = getDefaultInterfaceFormat(config.controllerFamily);
  currentCmd = 0x00;
  cmdDataLen = 0;
  casetXS = 0;
  casetXE = config.buffWidth - 1;
  rasetYS = 0;
  rasetYE = config.buffHeight - 1;
  ramwrX = 0;
  ramwrY = 0;
  ramwrBufLen = 0;
  if (config.trimMode == TrimMode::AUTO) {
    config.trimX = 0;
    config.trimY = 0;
    config.trimWidth = 0;
    config.trimHeight = 0;
  }
  updateWriteCache();
}

// Expand the trim area to include the specified rectangle (logical coordinates)
void ControllerBase::expandTrimX(uint16_t x0, uint16_t x1) {
  if (config.trimMode != TrimMode::AUTO) return;
  if (x0 >= x1) return;                // empty rectangle
  if (x1 >= config.buffWidth) return;  // out of bounds
  if (config.trimWidth == 0) {
    // No trim area yet; set to the new rectangle
    config.trimX = x0;
    config.trimWidth = static_cast<uint16_t>(x1 - x0 + 1);
  } else {
    config.trimX = std::min(config.trimX, x0);
    config.trimWidth =
        std::max(config.trimWidth, (uint16_t)(x1 - config.trimX + 1));
  }
  calcScaleParams();
}

// Expand the trim area to include the specified rectangle (logical coordinates)
void ControllerBase::expandTrimY(uint16_t y0, uint16_t y1) {
  if (config.trimMode != TrimMode::AUTO) return;
  if (y0 >= y1) return;                 // empty rectangle
  if (y1 >= config.buffHeight) return;  // out of bounds
  if (config.trimHeight == 0) {
    // No trim area yet; set to the new rectangle
    config.trimY = y0;
    config.trimHeight = static_cast<uint16_t>(y1 - y0 + 1);
  } else {
    config.trimY = std::min(config.trimY, y0);
    config.trimHeight =
        std::max(config.trimHeight, (uint16_t)(y1 - config.trimY + 1));
  }
  calcScaleParams();
}

// Process all RAMWR data at once (moves switch(interfaceFormat) outside the
// loop)
void ControllerBase::processRamwrData(const uint8_t* data, uint32_t numBytes,
                                      uint32_t stride) {
  int32_t i = 0;
  int32_t length = numBytes * stride;

  InterfaceFormat effectiveFmt =
      (config.interfaceFormatOverride >= 0)
          ? static_cast<InterfaceFormat>(config.interfaceFormatOverride)
          : interfaceFormat;
  switch (effectiveFmt) {
    case InterfaceFormat::RGB332: {
      // 1 byte = 1 pixel: {R[2:0], G[2:0], B[1:0]}
      // Expand 3→5 bits: (r<<2)|(r>>1)  3→6 bits: (g<<3)|g  2→5 bits:
      // (b<<3)|(b<<1)|(b>>1)
      while (i < length) {
        uint8_t b = data[i];
        i += stride;
        uint_fast16_t r5 = (b >> 5) & 0x07u;
        uint_fast16_t g3 = (b >> 2) & 0x07u;
        uint_fast16_t b2 = b & 0x03u;
        r5 = (r5 << 2) | (r5 >> 1);
        uint_fast16_t g6 = (g3 << 3) | g3;
        uint_fast16_t b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
        writePixelRgb565(static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5));
      }
      break;
    }

    case InterfaceFormat::RGB111_HPACK2_H2L_RA8: {
      // 1 byte = 2 pixels: bits[5:3] = younger pixel, bits[2:0] = older pixel
      // 3-bit RGB -> RGB565 lookup (R:1->5bit, G:1->6bit, B:1->5bit)
      static const uint16_t kLut[8] = {
          0x0000, 0x001F, 0x07E0, 0x07FF, 0xF800, 0xF81F, 0xFFE0, 0xFFFF,
      };
      while (i < length) {
        uint8_t b = data[i];
        i += stride;
        writePixelRgb565(kLut[(b >> 3) & 0x07u]);
        writePixelRgb565(kLut[b & 0x07u]);
      }
      break;
    }

    case InterfaceFormat::RGB444_HPACK2_H2L_BE:
      // byte0: R1[3:0] G1[3:0]  byte1: B1[3:0] R2[3:0]  byte2: G2[3:0] B2[3:0]
      // 4bit→5bit: x<<1 (MSB-align; LSB zeroed)  4bit→6bit: x<<2 (MSB-align;
      // lower 2 bits zeroed) Drain leftover bytes (0–2 bytes)
      while (ramwrBufLen > 0 && i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
        if (ramwrBufLen == 3) {
          uint_fast16_t b0 = ramwrBuf[0], b1 = ramwrBuf[1], b2 = ramwrBuf[2];
          uint_fast16_t pixel0 = 0;
          pixel0 |= (b0 << 8) & 0xF000;  // R1
          pixel0 |= (b0 << 7) & 0x0780;  // G1
          pixel0 |= (b1 >> 3) & 0x001E;  // B1
          writePixelRgb565(pixel0);
          uint_fast16_t pixel1 = 0;
          pixel1 |= (b1 << 12) & 0xF000;  // R2
          pixel1 |= (b2 << 3) & 0x0780;   // G2
          pixel1 |= (b2 << 1) & 0x001E;   // B2
          writePixelRgb565(pixel1);
          ramwrBufLen = 0;
        }
      }
      // Tight loop: 3 bytes → 2 pixels
      while (i + stride * 3 <= length) {
        uint_fast16_t b0 = data[i];
        i += stride;
        uint_fast16_t b1 = data[i];
        i += stride;
        uint_fast16_t b2 = data[i];
        i += stride;
        uint_fast16_t pixel0 = 0;
        pixel0 |= (b0 << 8) & 0xF000;  // R1
        pixel0 |= (b0 << 7) & 0x0780;  // G1
        pixel0 |= (b1 >> 3) & 0x001E;  // B1
        writePixelRgb565(pixel0);
        uint_fast16_t pixel1 = 0;
        pixel1 |= (b1 << 12) & 0xF000;  // R2
        pixel1 |= (b2 << 3) & 0x0780;   // G2
        pixel1 |= (b2 << 1) & 0x001E;   // B2
        writePixelRgb565(pixel1);
      }
      // Save remainder
      while (i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
      }
      break;

    case InterfaceFormat::RGB565_BE:
      // Drain leftover 1 byte
      if (ramwrBufLen == 1 && i < length) {
        uint_fast16_t b0 = ramwrBuf[0];
        uint_fast16_t b1 = data[i];
        uint_fast16_t pixel =
            cachedLittleEndian ? (b0 | (b1 << 8)) : ((b0 << 8) | b1);
        writePixelRgb565(pixel);
        i += stride;
        ramwrBufLen = 0;
      }
      // Tight loop: 2 bytes → 1 pixel
      while (i + stride * 2 <= length) {
        uint_fast16_t b0 = data[i];
        i += stride;
        uint_fast16_t b1 = data[i];
        i += stride;
        uint_fast16_t pixel =
            cachedLittleEndian ? (b0 | (b1 << 8)) : ((b0 << 8) | b1);
        writePixelRgb565(pixel);
      }
      // Save remainder
      if (i < length) {
        ramwrBuf[0] = data[i];
        ramwrBufLen = 1;
      }
      break;

    case InterfaceFormat::RGB666_UNPACK_LA8_BE:
      // Upper 6 bits of each byte are significant (lower 2 bits = 0)
      // R5 = byte0>>3, G6 = byte1>>2, B5 = byte2>>3 maps directly to RGB565
      // Drain leftover bytes (0–2 bytes)
      while (ramwrBufLen > 0 && i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
        if (ramwrBufLen == 3) {
          uint_fast16_t pixel = 0;
          pixel |= (ramwrBuf[0] & 0xFCu) << 8;  // R5
          pixel |= (ramwrBuf[1] & 0xFCu) << 3;  // G6
          pixel |= (ramwrBuf[2] & 0xFCu) >> 3;  // B5
          writePixelRgb565(static_cast<uint16_t>(pixel));
          ramwrBufLen = 0;
        }
      }
      // Tight loop: 3 bytes → 1 pixel
      while (i + stride * 3 <= length) {
        uint_fast16_t pixel = 0;
        pixel |= (data[i] & 0xFCu) << 8;  // R5
        i += stride;
        pixel |= (data[i] & 0xFCu) << 3;  // G6
        i += stride;
        pixel |= (data[i] & 0xFCu) >> 3;  // B5
        i += stride;
        writePixelRgb565(static_cast<uint16_t>(pixel));
      }
      // Save remainder
      while (i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
      }
      break;

    case InterfaceFormat::RGB666_UNPACK_RA8_BE:
      // 3 bytes = 1 pixel: color data in bits[5:0] of each byte (right-aligned)
      // R5 = byte0[5:1], G6 = byte1[5:0], B5 = byte2[5:1]
      // (bit[0] of R and B channels is hardware-unused on SSD1331)
      // Drain leftover bytes (0-2 bytes)
      while (ramwrBufLen > 0 && i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
        if (ramwrBufLen == 3) {
          uint_fast16_t pixel = 0;
          pixel |= static_cast<uint_fast16_t>((ramwrBuf[0] >> 1) & 0x1Fu)
                   << 11;                                                 // R5
          pixel |= static_cast<uint_fast16_t>(ramwrBuf[1] & 0x3Fu) << 5;  // G6
          pixel |=
              static_cast<uint_fast16_t>((ramwrBuf[2] >> 1) & 0x1Fu);  // B5
          writePixelRgb565(static_cast<uint16_t>(pixel));
          ramwrBufLen = 0;
        }
      }
      // Tight loop: 3 bytes → 1 pixel
      while (i + stride * 3 <= length) {
        uint_fast16_t pixel = 0;
        pixel |= static_cast<uint_fast16_t>((data[i] >> 1) & 0x1Fu)
                 << 11;  // R5
        i += stride;
        pixel |= static_cast<uint_fast16_t>(data[i] & 0x3Fu) << 5;  // G6
        i += stride;
        pixel |= static_cast<uint_fast16_t>((data[i] >> 1) & 0x1Fu);  // B5
        i += stride;
        writePixelRgb565(static_cast<uint16_t>(pixel));
      }
      // Save remainder
      while (i < length) {
        ramwrBuf[ramwrBufLen++] = data[i];
        i += stride;
      }
      break;
  }
}

// Process a data byte stream: RAMWR data is handled in bulk, others byte by
// byte
void ControllerBase::feedData(const uint8_t* data, uint32_t numBytes,
                              uint32_t stride) {
  if (stride == 0) stride = 1;  // safety guard
  if (isRamWriteCommand()) {
    if (!writeProtected) processRamwrData(data, numBytes, stride);
  } else {
    for (uint32_t i = 0; i < numBytes * stride; i += stride) {
      feedDataByte(data[i]);
    }
  }
}

//=============================================================================
// LcdTap
//=============================================================================

LcdTap::LcdTap(const LcdTapConfig& config, const HostInterface& host)
    : impl_(nullptr) {
  if (!host.alloc || !host.free) return;

  ControllerBase* ctrl = nullptr;
  switch (config.controllerFamily) {
    case ControllerFamily::ST7789:
      ctrl = new (std::nothrow) St7789Controller();
      break;
    case ControllerFamily::SSD1306:
      ctrl = new (std::nothrow) Ssd1306Controller();
      break;
    case ControllerFamily::SSD1331:
      ctrl = new (std::nothrow) Ssd1331Controller();
      break;
    case ControllerFamily::ILI9341:
      ctrl = new (std::nothrow) Ili9341Controller();
      break;
  }
  if (!ctrl) return;

  ctrl->config = config;
  ctrl->host = host;
  ctrl->status = Status::OK;
  ctrl->hwReset = false;
  ctrl->framebuf = nullptr;
  ctrl->outputRotation = config.outputRotation & 3u;

  if (config.buffWidth == 0 || config.buffHeight == 0 || config.dviWidth == 0 ||
      config.dviHeight == 0) {
    ctrl->status = Status::INVALID_PARAM;
    impl_ = ctrl;
    return;
  }

  size_t fbSize =
      (size_t)config.buffWidth * config.buffHeight * sizeof(uint16_t);
  ctrl->framebuf = static_cast<uint16_t*>(host.alloc(fbSize));
  if (!ctrl->framebuf) {
    ctrl->status = Status::OUT_OF_MEMORY;
    impl_ = ctrl;
    return;
  }

  impl_ = ctrl;
  ctrl->calcScaleParams();
  ctrl->softReset();
}

LcdTap::~LcdTap() {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->framebuf) ctrl->host.free(ctrl->framebuf);
  delete ctrl;
}

Status LcdTap::getStatus() const {
  if (!impl_) return Status::OUT_OF_MEMORY;
  return static_cast<const ControllerBase*>(impl_)->status;
}

void LcdTap::inputReset(bool assert) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK) return;
  ctrl->hwReset = assert;
  if (!assert) {
    if (dumpState_ == DumpState::ACTIVE) dumpPush(DUMP_EVENT_HW_RESET);
    ctrl->softReset();
    ctrl->log("HW RESET released");
  }
}

void LcdTap::inputCommand(uint8_t byte) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK || ctrl->hwReset) return;
  ctrl->dispatchCommand(byte);
  if (dumpState_ == DumpState::ACTIVE) dumpPush(byte);
}

void LcdTap::inputData(const uint8_t* data, uint32_t numBytes,
                       uint32_t stride) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK || ctrl->hwReset) return;
  ctrl->feedData(data, numBytes, stride);
  if (dumpState_ == DumpState::ACTIVE) {
    const uint32_t s = (stride == 0) ? 1u : stride;
    for (uint32_t i = 0; i < numBytes && dumpState_ == DumpState::ACTIVE; ++i)
      dumpPush(0x100u | data[i * s]);
  }
}

//=============================================================================
// LcdTap dump API
//=============================================================================

void LcdTap::dumpStart(const DumpConfig& dumpCfg) {
  dumpConfig_ = dumpCfg;
  dumpBuffSize_ = 0;
  dumpState_ = DumpState::WAIT;
}

DumpState LcdTap::dumpGetState() const { return dumpState_; }

uint16_t LcdTap::dumpGetSize() const { return dumpBuffSize_; }

void LcdTap::dumpForceTrigger() {
  if (dumpState_ == DumpState::WAIT) dumpState_ = DumpState::ACTIVE;
}

void LcdTap::dumpAbort() { dumpState_ = DumpState::COMPLETE; }

const uint16_t* LcdTap::dumpGetBuffer() const { return dumpBuffer_; }

void LcdTap::fillScanline(uint16_t dviLine, uint16_t* dst) const {
  if (!impl_) return;
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  if (ctrl->status != Status::OK || !ctrl->framebuf) return;

  const uint16_t dviW = ctrl->config.dviWidth;

  // Display off, sleeping, or in the vertical black border region
  bool powerOff =
      !ctrl->config.forcePowerOn && (ctrl->sleeping || !ctrl->displayOn);
  if (powerOff || dviLine < ctrl->outDestY ||
      dviLine >= ctrl->outDestY + ctrl->outDestH) {
    memset(dst, 0, dviW * sizeof(uint16_t));
    return;
  }

  // Vertical mapping: compute LCD row via fixed-point multiply
  const uint32_t lcdRowOut =
      (((uint32_t)(dviLine - ctrl->outDestY) * ctrl->outSrcStepV) >> 16);

  uint16_t* d = dst;

  // Left black border
  memset(d, 0, (size_t)ctrl->outDestX * sizeof(uint16_t));
  d += ctrl->outDestX;

  // Active area: horizontal scaling + brightness inversion + rotation
  // Inversion: XOR all RGB565 bits → (31-R, 63-G, 31-B) inverts each channel
  const uint16_t inv = ctrl->inverted ? 0xFFFFu : 0u;
  const uint16_t* fb = ctrl->framebuf;
  const uint32_t stride = ctrl->config.buffWidth;
  const uint32_t srcX = ctrl->outSrcX;
  const uint32_t srcY = ctrl->outSrcY;
  const uint32_t srcW = ctrl->outSrcW;
  const uint32_t srcH = ctrl->outSrcH;
  const uint32_t srcR = srcX + srcW - 1;
  const uint32_t srcB = srcY + srcH - 1;
  uint32_t hAccum = 0;

  switch (ctrl->outputRotation) {
    default:
    case 0: {
      // rot=0: normal (row-major readout)
      const uint16_t* srcRow = fb + (lcdRowOut + srcY) * stride;
      hAccum += srcX << 16;  // start at the left of the trim area
      for (uint16_t x = 0; x < ctrl->outDestW; ++x) {
        *d++ = srcRow[hAccum >> 16] ^ inv;
        hAccum += ctrl->outSrcStepH;
      }
      break;
    }
    case 1: {
      // rot=1: 90° CW  — (focusSrcY, lcdColOut) → ((trimH-1-lcdColOut),
      // focusSrcY)
      // Use a running pointer to avoid per-pixel multiply: start at the top of
      // the source column and step backward by stride as lcdColOut increases.
      uint32_t prevCol = 0;
      const uint16_t* cur = fb + srcB * stride + srcX + lcdRowOut;
      for (uint16_t x = 0; x < ctrl->outDestW; ++x) {
        uint32_t lcdColOut = hAccum >> 16;
        while (prevCol < lcdColOut) {
          cur -= stride;
          ++prevCol;
        }
        *d++ = *cur ^ inv;
        hAccum += ctrl->outSrcStepH;
      }
      break;
    }
    case 2: {
      // rot=2: 180° — (focusSrcY, lcdColOut) → ((trimH-1-focusSrcY),
      // (trimW-1-lcdColOut))
      const uint16_t* srcRow = fb + (uint32_t)(srcB - lcdRowOut) * stride;
      hAccum += (srcR << 16) + 0xFFFF;  // start at the right of the trim area
      for (uint16_t x = 0; x < ctrl->outDestW; ++x) {
        *d++ = srcRow[hAccum >> 16] ^ inv;
        hAccum -= ctrl->outSrcStepH;
      }
      break;
    }
    case 3: {
      // rot=3: 270° CW — (focusSrcY, lcdColOut) → (lcdColOut,
      // (physW-1-focusSrcY))
      // Use a running pointer to avoid per-pixel multiply: start at the bottom
      // of the source column and step forward by physW as lcdColOut increases.
      uint32_t prevCol = 0;
      const uint16_t* cur = fb + srcY * stride + (srcR - lcdRowOut);
      for (uint16_t x = 0; x < ctrl->outDestW; ++x) {
        uint32_t lcdColOut = hAccum >> 16;
        while (prevCol < lcdColOut) {
          cur += stride;
          ++prevCol;
        }
        *d++ = *cur ^ inv;
        hAccum += ctrl->outSrcStepH;
      }
      break;
    }
  }

  // Right black border
  memset(d, 0,
         (size_t)(dviW - ctrl->outDestX - ctrl->outDestW) * sizeof(uint16_t));
}

LcdTapConfig LcdTap::getConfig() const {
  if (!impl_) return {};
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  LcdTapConfig cfg = ctrl->config;
  cfg.outputRotation = ctrl->outputRotation;
  return cfg;
}

Status LcdTap::updateConfig(const LcdTapConfig& cfg) {
  if (!impl_) return Status::NOT_READY;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK) return ctrl->status;

  if (cfg.buffWidth == 0 || cfg.buffHeight == 0 || cfg.dviWidth == 0 ||
      cfg.dviHeight == 0) {
    return Status::INVALID_PARAM;
  }

  // If the controller type changes, destroy the old controller and create a new
  // one.  The new controller starts from a clean state (sleeping/displayOn are
  // not preserved because the new controller has not yet received any
  // commands).
  if (cfg.controllerFamily != ctrl->config.controllerFamily) {
    HostInterface host = ctrl->host;
    if (ctrl->framebuf) host.free(ctrl->framebuf);
    delete ctrl;
    impl_ = nullptr;

    switch (cfg.controllerFamily) {
      case ControllerFamily::ST7789:
        ctrl = new (std::nothrow) St7789Controller();
        break;
      case ControllerFamily::SSD1306:
        ctrl = new (std::nothrow) Ssd1306Controller();
        break;
      case ControllerFamily::SSD1331:
        ctrl = new (std::nothrow) Ssd1331Controller();
        break;
      case ControllerFamily::ILI9341:
        ctrl = new (std::nothrow) Ili9341Controller();
        break;
      default: return Status::OUT_OF_MEMORY;
    }
    if (!ctrl) return Status::OUT_OF_MEMORY;

    ctrl->host = host;
    ctrl->status = Status::OK;
    ctrl->hwReset = false;
    ctrl->framebuf = nullptr;
    impl_ = ctrl;

    size_t fbSize = (size_t)cfg.buffWidth * cfg.buffHeight * sizeof(uint16_t);
    ctrl->framebuf = static_cast<uint16_t*>(host.alloc(fbSize));
    if (!ctrl->framebuf) return Status::OUT_OF_MEMORY;

    ctrl->config = cfg;
    ctrl->outputRotation = cfg.outputRotation & 3u;
    ctrl->calcScaleParams();
    ctrl->softReset();
    return Status::OK;
  }

  // Same controller type: update config in-place.
  // Allocate new framebuffer before freeing the old one.
  size_t currFbSize = (size_t)ctrl->config.buffWidth * ctrl->config.buffHeight *
                      sizeof(uint16_t);
  size_t newFbSize = (size_t)cfg.buffWidth * cfg.buffHeight * sizeof(uint16_t);
  if (newFbSize > currFbSize) {
    uint16_t* newFb = static_cast<uint16_t*>(ctrl->host.alloc(newFbSize));
    if (!newFb) return Status::OUT_OF_MEMORY;
    ctrl->host.free(ctrl->framebuf);
    ctrl->framebuf = newFb;
  }

  bool sleeping = ctrl->sleeping;
  bool displayOn = ctrl->displayOn;
  ctrl->config = cfg;
  ctrl->outputRotation = cfg.outputRotation & 3u;
  ctrl->calcScaleParams();
  ctrl->softReset();
  ctrl->sleeping = sleeping;
  ctrl->displayOn = displayOn;
  return Status::OK;
}

bool LcdTap::isOutputInverted() const {
  if (!impl_) return false;
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  return ctrl->inverted;
}

bool LcdTap::isOutputSwapRB() const {
  if (!impl_) return false;
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  return ctrl->cachedBGR;
}

uint16_t* LcdTap::getFramebuf() {
  if (!impl_) return nullptr;
  return static_cast<ControllerBase*>(impl_)->framebuf;
}

void LcdTap::getOutSrcRegion(uint16_t* x, uint16_t* y, uint16_t* w,
                             uint16_t* h) const {
  if (!impl_) {
    *x = *y = *w = *h = 0;
    return;
  }
  const ControllerBase* ctrl = static_cast<const ControllerBase*>(impl_);
  *x = ctrl->outSrcX;
  *y = ctrl->outSrcY;
  *w = ctrl->outSrcW;
  *h = ctrl->outSrcH;
}

void LcdTap::setWriteProtected(bool protect) {
  if (!impl_) return;
  static_cast<ControllerBase*>(impl_)->writeProtected = protect;
}

bool LcdTap::isWriteProtected() const {
  if (!impl_) return false;
  return static_cast<const ControllerBase*>(impl_)->writeProtected;
}

void LcdTap::setDisplayOn(bool on) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK) return;
  ctrl->sleeping = !on;
  ctrl->displayOn = on;
}

void LcdTap::setOutputRotation(int rot) {
  if (!impl_) return;
  ControllerBase* ctrl = static_cast<ControllerBase*>(impl_);
  if (ctrl->status != Status::OK) return;
  ctrl->config.outputRotation = rot & 3u;
  ctrl->outputRotation = ctrl->config.outputRotation;
  ctrl->calcScaleParams();
}

}  // namespace lcdtap
