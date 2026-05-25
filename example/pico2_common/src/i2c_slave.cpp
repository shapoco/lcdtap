#include <initializer_list>

#include "lcdtap/pico2/i2c_slave.hpp"

#include <hardware/gpio.h>
#include <hardware/i2c.h>
#include <hardware/irq.h>
#include <hardware/regs/i2c.h>
#include <hardware/structs/i2c.h>

namespace lcdtap::pico2 {

static I2cSlaveState *sI2c = nullptr;

static void __not_in_flash_func(i2cIrqHandler)() {
  I2cSlaveState *s = sI2c;
  i2c_hw_t *hw = i2c_get_hw(s->cfg.i2c);
  uint32_t intr = hw->intr_stat;

  if (intr & I2C_IC_INTR_STAT_R_RX_FULL_BITS) {
    uint32_t raw = hw->data_cmd;
    bool firstByte = (raw & I2C_IC_DATA_CMD_FIRST_DATA_BYTE_BITS) != 0u;
    uint8_t byte = static_cast<uint8_t>(raw & 0xFFu);

    if (firstByte) {
      s->rxState = I2cRxState::WAIT_CTRL;
    }

    if (s->rxState == I2cRxState::WAIT_CTRL) {
      // Decode SSD1306 control byte: bit6=D/C#, bit7=Co (Co=1 not supported)
      bool dc = (byte >> 6u) & 1u;
      s->rxState = dc ? I2cRxState::STREAM_DATA : I2cRxState::STREAM_CMD;
    } else {
      uint32_t word = (s->rxState == I2cRxState::STREAM_DATA)
                          ? (0x100u | byte)
                          : static_cast<uint32_t>(byte);
      s->ringBuf[s->writeIdx] = word;
      s->writeIdx = (s->writeIdx + 1u) & (s->ringWords - 1u);
    }
  }

  if (intr & I2C_IC_INTR_STAT_R_STOP_DET_BITS) {
    s->rxState = I2cRxState::WAIT_CTRL;
    (void)hw->clr_stop_det;
  }

  // Respond to read requests with a dummy byte (read-back not supported)
  if (intr & I2C_IC_INTR_STAT_R_RD_REQ_BITS) {
    hw->data_cmd = 0xFFu;
    (void)hw->clr_rd_req;
  }

  if (intr & I2C_IC_INTR_STAT_R_TX_ABRT_BITS) {
    (void)hw->clr_tx_abrt;
  }
}

void i2cSlaveInit(I2cSlaveState *s, const I2cSlaveConfig &cfg,
                  uint32_t *ringBuf, uint32_t ringWords) {
  sI2c = s;
  s->cfg = cfg;
  s->ringBuf = ringBuf;
  s->ringWords = ringWords;
  s->writeIdx = 0;
  s->readIdx = 0;
  s->rxState = I2cRxState::WAIT_CTRL;

  i2c_init(cfg.i2c, 400u * 1000u);  // baudrate irrelevant in slave mode
  gpio_set_function(cfg.pinSda, GPIO_FUNC_I2C);
  gpio_set_function(cfg.pinScl, GPIO_FUNC_I2C);
  gpio_pull_up(cfg.pinSda);
  gpio_pull_up(cfg.pinScl);

  i2c_set_slave_mode(cfg.i2c, true, cfg.slaveAddr);

  i2c_get_hw(cfg.i2c)->intr_mask =
      I2C_IC_INTR_MASK_M_RX_FULL_BITS | I2C_IC_INTR_MASK_M_STOP_DET_BITS |
      I2C_IC_INTR_MASK_M_RD_REQ_BITS | I2C_IC_INTR_MASK_M_TX_ABRT_BITS;

  uint irqNum = (cfg.i2c == i2c0) ? I2C0_IRQ : I2C1_IRQ;
  irq_set_exclusive_handler(irqNum, i2cIrqHandler);
  // Elevate I2C IRQ above default priority so it can preempt other IRQs.
  // Without this, equal-priority IRQs cannot preempt each other on
  // Cortex-M33, which can delay I2C draining, fill the 16-entry FIFO, and
  // cause clock stretching → master timeout → transaction abort → desync.
  irq_set_priority(irqNum, PICO_DEFAULT_IRQ_PRIORITY >> 1);
  irq_set_enabled(irqNum, true);
}

void i2cSlaveDeinit(I2cSlaveState *s) {
  uint irqNum = (s->cfg.i2c == i2c0) ? I2C0_IRQ : I2C1_IRQ;
  irq_set_enabled(irqNum, false);
  i2c_deinit(s->cfg.i2c);
  s->readIdx = 0;
  s->writeIdx = 0;
  s->rxState = I2cRxState::WAIT_CTRL;
}

void __not_in_flash_func(i2cSlaveProcess)(I2cSlaveState *s) {
  uint32_t writeIdx = s->writeIdx;  // snapshot of volatile

  if (!s->inst) {
    s->readIdx = writeIdx;
    return;
  }

  uint32_t dataStart = s->readIdx;
  while (s->readIdx != writeIdx) {
    uint32_t lastReadIdx = s->readIdx;
    uint32_t word = s->ringBuf[s->readIdx];
    s->readIdx = (s->readIdx + 1u) & (s->ringWords - 1u);

    if (word & 0x100u) {
      if (s->readIdx == 0) {
        s->inst->inputData((uint8_t *)&s->ringBuf[dataStart],
                           (s->ringWords - dataStart), sizeof(uint32_t));
        dataStart = 0;
      }
    } else {
      uint32_t dataLen = lastReadIdx - dataStart;
      if (dataLen != 0) {
        s->inst->inputData((uint8_t *)&s->ringBuf[dataStart], dataLen,
                           sizeof(uint32_t));
      }
      s->inst->inputCommand(static_cast<uint8_t>(word));
      dataStart = s->readIdx;
    }
  }

  uint32_t dataLen = s->readIdx - dataStart;
  if (dataLen != 0) {
    s->inst->inputData((uint8_t *)&s->ringBuf[dataStart], dataLen,
                       sizeof(uint32_t));
  }
}

}  // namespace pico2
