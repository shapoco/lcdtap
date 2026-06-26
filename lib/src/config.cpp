#include "lcdtap/config.hpp"

#include <algorithm>
#include <cstdio>

namespace lcdtap {

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
  }
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
    default: return InterfaceFormat::RGB565_BE;
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
      e->options = BUS_NAMES;
      e->max = static_cast<int16_t>(BusType::NUM_BUSES) - 1;
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
    case ConfigId::BUFF_WIDTH: return static_cast<int16_t>(cfg.buffWidth);
    case ConfigId::BUFF_HEIGHT: return static_cast<int16_t>(cfg.buffHeight);
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
    case ConfigId::BUFF_WIDTH:
      cfg->buffWidth = static_cast<uint16_t>(value);
      break;
    case ConfigId::BUFF_HEIGHT:
      cfg->buffHeight = static_cast<uint16_t>(value);
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
    default: buf[0] = '\0'; break;
  }
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

  if (cfg->trimMode != TrimMode::CUSTOM) {
    cfg->trimX = 0;
    cfg->trimY = 0;
    cfg->trimWidth = cfg->buffWidth;
    cfg->trimHeight = cfg->buffHeight;
  }
}

//=============================================================================
// Command dump
//=============================================================================
DumpConfig getDefaultDumpConfig() { return {}; }

}  // namespace lcdtap
