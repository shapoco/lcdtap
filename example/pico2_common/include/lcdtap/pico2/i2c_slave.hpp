#pragma once
#include <cstdint>

#include "hardware/i2c.h"
#include "lcdtap/lcdtap.hpp"

namespace lcdtap::pico2 {

struct I2cSlaveConfig {
  i2c_inst_t *i2c;
  uint pinSda;
  uint pinScl;
  uint8_t slaveAddr;
};

enum class I2cRxState : uint8_t { WAIT_CTRL, STREAM_CMD, STREAM_DATA };

struct I2cSlaveState {
  // Set by caller before i2cSlaveProcess calls:
  lcdtap::LcdTap *inst;
  // Internal fields:
  I2cSlaveConfig cfg;
  uint32_t *ringBuf;
  uint32_t ringWords;  // must be a power of 2
  volatile uint32_t writeIdx;
  uint32_t readIdx;
  volatile I2cRxState rxState;
};

// Initialize I2C peripheral in slave mode and enable IRQ.
// ringBuf is a caller-allocated static array; ringWords must be a power of 2.
// IRQ priority is elevated above default to avoid clock-stretching under load.
// Only one I2cSlaveState instance can be active at a time.
void i2cSlaveInit(I2cSlaveState *s, const I2cSlaveConfig &cfg,
                  uint32_t *ringBuf, uint32_t ringWords);

// Disable I2C IRQ and deinit peripheral.
void i2cSlaveDeinit(I2cSlaveState *s);

// Drain the ring buffer and dispatch commands/data to s->inst.
void __not_in_flash_func(i2cSlaveProcess)(I2cSlaveState *s);

}  // namespace pico2
