#include "lcdtap/config.hpp"

#include <algorithm>
#include <cstdio>

namespace lcdtap {

static uint8_t presetRotationOffset = 0;

//=============================================================================
// getDefaultConfig
//=============================================================================
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
  cfg->i2cSlaveAddr = 0x3C;
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
      cfg->inverted = true;
      cfg->swapRB = true;
      cfg->outputRotation = 0;
      cfg->busInterface = BusType::SPI_4LINE;
      break;
    case ControllerFamily::ST7032:
      cfg->textCols = 16;
      cfg->textRows = 2;
      cfg->textCgramArea = 1;
      cfg->outputRotation = 0;
      cfg->busInterface = BusType::I2C;
      cfg->i2cSlaveAddr = 0x3E;
      break;
    default: break;
  }
  normalizeConfig(cfg);
  cfg->trimMode = TrimMode::OFF;
  cfg->trimX = 0;
  cfg->trimY = 0;
  cfg->trimWidth = cfg->buffWidth;
  cfg->trimHeight = cfg->buffHeight;
}

InterfaceFormat getDefaultInterfaceFormat(ControllerFamily type) {
  switch (type) {
    case ControllerFamily::ST7789: return InterfaceFormat::RGB565_BE;
    case ControllerFamily::SSD1306: return InterfaceFormat::GRAY1_VPACK8_H2L;
    case ControllerFamily::SSD1331: return InterfaceFormat::RGB332;
    case ControllerFamily::ILI9341: return InterfaceFormat::RGB565_BE;
    // ST7032 carries character codes, not pixels; value is unused.
    case ControllerFamily::ST7032: return InterfaceFormat::GRAY1_VPACK8_H2L;
    default: return InterfaceFormat::RGB565_BE;
  }
}

void normalizeConfig(LcdTapConfig* cfg) {
  cfg->i2cSlaveAddr &= 0x7F;
  if (cfg->controllerFamily != ControllerFamily::ST7032) return;

  uint8_t cols = cfg->textCols & ~1u;  // even only
  cfg->textCols = (cols < 2) ? 2 : (cols > 40) ? 40 : cols;
  uint8_t rows = cfg->textRows;
  cfg->textRows = (rows >= 4) ? 4 : (rows >= 2) ? 2 : 1;
  cfg->textCgramArea &= 3;

  // Character cell is 5x8 plus a 1px gap; no gap after the last column/row.
  cfg->buffWidth = cfg->textCols * 6 - 1;
  cfg->buffHeight = cfg->textRows * 9 - 1;
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
      e->options = BUS_NAMES;
      e->max = static_cast<int16_t>(BusType::NUM_BUSES) - 1;
      break;

    // I2C Slave Address
    case ConfigId::I2C_ADDR:
      e->type = ValueType::HEX;
      e->name = "I2C Address";
      e->min = 0x08;
      e->max = 0x77;

      e->enableKeyId = static_cast<int16_t>(ConfigId::BUS_INTERFACE);
      e->enableKeyValueMin = static_cast<int16_t>(BusType::I2C);
      e->enableKeyValueMax = static_cast<int16_t>(BusType::I2C);
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

      // Disabled for ST7032: the framebuffer size is derived from cols/rows.
      e->enableKeyId = static_cast<int16_t>(ConfigId::CTRL_FAMILY);
      e->enableKeyValueMin = static_cast<int16_t>(ControllerFamily::ST7789);
      e->enableKeyValueMax = static_cast<int16_t>(ControllerFamily::ILI9341);
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

      // Disabled for ST7032: the framebuffer size is derived from cols/rows.
      e->enableKeyId = static_cast<int16_t>(ConfigId::CTRL_FAMILY);
      e->enableKeyValueMin = static_cast<int16_t>(ControllerFamily::ST7789);
      e->enableKeyValueMax = static_cast<int16_t>(ControllerFamily::ILI9341);
      break;

    // ST7032 Columns
    case ConfigId::TEXT_COLS:
      e->type = ValueType::INT16;
      e->name = "Text Buff Cols";
      e->min = 2;
      e->max = 40;
      e->step = 2;

      e->enableKeyId = static_cast<int16_t>(ConfigId::CTRL_FAMILY);
      e->enableKeyValueMin = static_cast<int16_t>(ControllerFamily::ST7032);
      e->enableKeyValueMax = static_cast<int16_t>(ControllerFamily::ST7032);
      break;

    // ST7032 Rows
    case ConfigId::TEXT_ROWS:
      e->type = ValueType::ENUM;
      e->name = "Text Buff Rows";
      e->options = ST7032_ROWS_NAMES;
      e->max = 2;

      e->enableKeyId = static_cast<int16_t>(ConfigId::CTRL_FAMILY);
      e->enableKeyValueMin = static_cast<int16_t>(ControllerFamily::ST7032);
      e->enableKeyValueMax = static_cast<int16_t>(ControllerFamily::ST7032);
      break;

    // ST7032 OPR (CGROM/CGRAM option)
    case ConfigId::TEXT_CGRAM_AREA:
      e->type = ValueType::ENUM;
      e->name = "Text CGRAM Area";
      e->options = ST7032_CGRAM_NAMES;
      e->max = 3;

      e->enableKeyId = static_cast<int16_t>(ConfigId::CTRL_FAMILY);
      e->enableKeyValueMin = static_cast<int16_t>(ControllerFamily::ST7032);
      e->enableKeyValueMax = static_cast<int16_t>(ControllerFamily::ST7032);
      break;

    // Inverse
    case ConfigId::INVERSE:
      e->type = ValueType::BOOL;
      e->name = "Inverse";
      e->options = ON_OFF_NAMES;
      break;

    // Swap Red/Blue
    case ConfigId::SWAP_RB:
      e->type = ValueType::BOOL;
      e->name = "Swap Red/Blue";
      e->options = ON_OFF_NAMES;

      // Enable for pixel-based controllers only (no-op for SSD1306;
      // meaningless for the ST7032 character display).
      e->enableKeyId = static_cast<int16_t>(ConfigId::CTRL_FAMILY);
      e->enableKeyValueMin = static_cast<int16_t>(ControllerFamily::ST7789);
      e->enableKeyValueMax = static_cast<int16_t>(ControllerFamily::ILI9341);
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

      // Enable for pixel-based controllers only (no-op for SSD1306;
      // meaningless for the ST7032 character display).
      e->enableKeyId = static_cast<int16_t>(ConfigId::CTRL_FAMILY);
      e->enableKeyValueMin = static_cast<int16_t>(ControllerFamily::ST7789);
      e->enableKeyValueMax = static_cast<int16_t>(ControllerFamily::ILI9341);
      break;

    // Trim Mode
    case ConfigId::TRIM_MODE:
      e->type = ValueType::ENUM;
      e->name = "Trim Mode";
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

    // Flip Mode
    case ConfigId::FLIP_MODE:
      e->type = ValueType::ENUM;
      e->name = "Flip Mode";
      e->options = FLIP_MODE_NAMES;
      e->max = static_cast<int16_t>(FlipMode::FLIP_HV);
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
    case ConfigId::I2C_ADDR: return static_cast<int16_t>(cfg.i2cSlaveAddr);
    case ConfigId::BUFF_WIDTH: return static_cast<int16_t>(cfg.buffWidth);
    case ConfigId::BUFF_HEIGHT: return static_cast<int16_t>(cfg.buffHeight);
    case ConfigId::TEXT_COLS: return static_cast<int16_t>(cfg.textCols);
    case ConfigId::TEXT_ROWS:
      // Actual row count 1/2/4 -> enum index 0/1/2
      return (cfg.textRows >= 4) ? 2 : (cfg.textRows >= 2) ? 1 : 0;
    case ConfigId::TEXT_CGRAM_AREA: return static_cast<int16_t>(cfg.textCgramArea);
    case ConfigId::INVERSE: return cfg.inverted ? 1 : 0;
    case ConfigId::SWAP_RB: return cfg.swapRB ? 1 : 0;
    case ConfigId::FORCE_PWR_ON: return cfg.forcePowerOn ? 1 : 0;
    case ConfigId::INTF_FMT_OVR:
      return static_cast<int16_t>(cfg.interfaceFormatOverride);
    case ConfigId::TRIM_MODE: return static_cast<int16_t>(cfg.trimMode);
    case ConfigId::TRIM_X: return static_cast<int16_t>(cfg.trimX);
    case ConfigId::TRIM_Y: return static_cast<int16_t>(cfg.trimY);
    case ConfigId::TRIM_WIDTH: return static_cast<int16_t>(cfg.trimWidth);
    case ConfigId::TRIM_HEIGHT: return static_cast<int16_t>(cfg.trimHeight);
    case ConfigId::FLIP_MODE: return static_cast<int16_t>(cfg.flipMode);
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
    case ConfigId::I2C_ADDR:
      cfg->i2cSlaveAddr = static_cast<uint8_t>(value & 0x7F);
      break;
    case ConfigId::BUFF_WIDTH:
      cfg->buffWidth = static_cast<uint16_t>(value);
      break;
    case ConfigId::BUFF_HEIGHT:
      cfg->buffHeight = static_cast<uint16_t>(value);
      break;
    case ConfigId::TEXT_COLS:
      cfg->textCols = static_cast<uint8_t>(value);
      break;
    case ConfigId::TEXT_ROWS:
      // Enum index 0/1/2 -> actual row count 1/2/4
      cfg->textRows = (value >= 2) ? 4 : (value >= 1) ? 2 : 1;
      break;
    case ConfigId::TEXT_CGRAM_AREA:
      cfg->textCgramArea = static_cast<uint8_t>(value & 3);
      break;
    case ConfigId::INVERSE: cfg->inverted = (value != 0); break;
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
    case ConfigId::FLIP_MODE:
      cfg->flipMode = static_cast<FlipMode>(value);
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
    case ValueType::HEX:
      snprintf(buf, static_cast<size_t>(bufLen), "0x%02X",
               static_cast<unsigned>(item.value) & 0xFFu);
      break;
    default: buf[0] = '\0'; break;
  }
}

void setPresetRotationOffset(uint8_t offset) {
  presetRotationOffset = offset & 3u;
}

//=============================================================================
// Get configuration preset
//=============================================================================
void getPresetConfig(ConfigPreset preset, LcdTapConfig* cfg) {
  switch (preset) {
    case ConfigPreset::ILI9341:
      getDefaultConfig(ControllerFamily::ILI9341, cfg);
      break;

    case ConfigPreset::ILI9342:
    case ConfigPreset::M5STACK_CORES3:
      getDefaultConfig(ControllerFamily::ILI9341, cfg);
      cfg->buffWidth = 320;
      cfg->buffHeight = 240;
      break;

    case ConfigPreset::ILI9488:
      getDefaultConfig(ControllerFamily::ILI9341, cfg);
      cfg->buffWidth = 320;
      cfg->buffHeight = 480;
      break;

    case ConfigPreset::SSD1306:
      getDefaultConfig(ControllerFamily::SSD1306, cfg);
      break;

    case ConfigPreset::SSD1331:
      getDefaultConfig(ControllerFamily::SSD1331, cfg);
      break;

    case ConfigPreset::TEXT_0802:
      getDefaultConfig(ControllerFamily::ST7032, cfg);
      cfg->textCols = 8;
      cfg->textRows = 2;
      cfg->textCgramArea = 2;
      break;

    case ConfigPreset::TEXT_1602:
      getDefaultConfig(ControllerFamily::ST7032, cfg);
      cfg->textCols = 16;
      cfg->textRows = 2;
      cfg->textCgramArea = 1;
      break;

    case ConfigPreset::TEXT_1604:
      getDefaultConfig(ControllerFamily::ST7032, cfg);
      cfg->textCols = 16;
      cfg->textRows = 4;
      cfg->textCgramArea = 0;
      cfg->busInterface = BusType::PARALLEL;
      break;

    case ConfigPreset::TEXT_2004:
      getDefaultConfig(ControllerFamily::ST7032, cfg);
      cfg->textCols = 20;
      cfg->textRows = 4;
      cfg->textCgramArea = 0;
      cfg->busInterface = BusType::PARALLEL;
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

    case ConfigPreset::ESPBOY:
      getDefaultConfig(ControllerFamily::ST7789, cfg);
      cfg->buffWidth = 136;
      cfg->buffHeight = 136;
      cfg->trimMode = TrimMode::CUSTOM;
      cfg->trimX = 6;
      cfg->trimY = 5;
      cfg->trimWidth = 128;
      cfg->trimHeight = 128;
      cfg->outputRotation = 2;
      break;

    case ConfigPreset::PICOPAD:
      getDefaultConfig(ControllerFamily::ST7789, cfg);
      cfg->outputRotation = 3;
      break;

    case ConfigPreset::PICOSYSTEM:
      getDefaultConfig(ControllerFamily::ST7789, cfg);
      cfg->buffWidth = 240;
      cfg->buffHeight = 240;
      cfg->busInterface = BusType::PARALLEL;
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

    case ConfigPreset::WIO_TERMINAL:
      getDefaultConfig(ControllerFamily::ILI9341, cfg);
      cfg->flipMode = FlipMode::FLIP_V;
      cfg->outputRotation = 3;
      break;

    case ConfigPreset::XIAMOCON:
      getDefaultConfig(ControllerFamily::ST7789, cfg);
      cfg->buffWidth = 240;
      cfg->buffHeight = 240;
      break;

    default: break;
  }

  // Re-derive the ST7032 framebuffer size from the preset's cols/rows before
  // the trim fixup below picks it up.
  normalizeConfig(cfg);

  if (cfg->trimMode != TrimMode::CUSTOM) {
    cfg->trimX = 0;
    cfg->trimY = 0;
    cfg->trimWidth = cfg->buffWidth;
    cfg->trimHeight = cfg->buffHeight;
  }

  cfg->outputRotation = (cfg->outputRotation + presetRotationOffset) & 3u;
}

//=============================================================================
// Command dump
//=============================================================================
DumpConfig getDefaultDumpConfig() { return {}; }

}  // namespace lcdtap
