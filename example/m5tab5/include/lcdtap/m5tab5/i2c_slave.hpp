#pragma once
#include <cstdint>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lcdtap/lcdtap.hpp"

namespace lcdtap::m5tab5 {

struct I2cSlaveConfig {
  int port;  // I2C peripheral number (must differ from M5Unified internal bus)
  int pinSda;
  int pinScl;
  uint8_t slaveAddr;
};

enum class I2cRxState : uint8_t { WAIT_CTRL, STREAM_CMD, STREAM_DATA };

struct I2cSlaveState {
  // Set by caller before i2cSlaveProcess calls:
  lcdtap::LcdTap *inst;
  // Task to notify from the ISR when new data arrives (set by caller):
  TaskHandle_t drainTask;
  // Internal fields:
  I2cSlaveConfig cfg;
  uint32_t *ringBuf;   // word = [bit8: D/C, bits7:0: data byte]
  uint32_t ringWords;  // must be a power of 2
  volatile uint32_t writeIdx;
  uint32_t readIdx;
  volatile I2cRxState rxState;
  volatile bool coSingleByte;  // Co=1: return to WAIT_CTRL after one byte
  volatile bool active;
  // Diagnostics (incremented in the ISR):
  volatile uint32_t isrCount;       // ISR invocations with a pending cause
  volatile uint32_t rxByteCount;    // bytes drained from the RX FIFO
  volatile uint32_t stopCount;      // TRANS_COMPLETE (STOP) interrupts
  volatile uint32_t stretchCount;   // clock-stretch causes handled
  volatile uint32_t startDetCount;  // START conditions detected
  volatile uint32_t unmatchCount;   // slave address compare mismatches
};

// Initialize the I2C peripheral in slave mode via direct i2c_ll register
// access (no IDF driver) and install an IRAM ISR pinned to core 0.
// SDA/SCL are wired through the GPIO matrix as open-drain input/output
// (i2c_set_pin must NOT be used). Clock stretching is disabled to stay
// compatible with SSD1306 masters that never check SCL.
// ringBuf is a caller-allocated array; ringWords must be a power of 2.
// Only one I2cSlaveState instance can be active at a time.
esp_err_t i2cSlaveInit(I2cSlaveState *s, const I2cSlaveConfig &cfg,
                       uint32_t *ringBuf, uint32_t ringWords);

// Disable the ISR and stop the peripheral.
void i2cSlaveDeinit(I2cSlaveState *s);

// Drain the ring buffer and dispatch commands/data to s->inst.
void i2cSlaveProcess(I2cSlaveState *s);

// Format a one-line diagnostic snapshot (counters + raw interrupt status
// + FIFO count + ring write index) into buf. Safe to call from any task.
void i2cSlaveDebugStatus(I2cSlaveState *s, char *buf, size_t len);

}  // namespace lcdtap::m5tab5
