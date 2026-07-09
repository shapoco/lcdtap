// I2C slave input driver for ESP32-P4.
//
// Drives the I2C peripheral directly through the hal i2c_ll_* inline
// functions (no IDF driver), based on lovyan03's ESP32_I2C_slave_example.
// The register-device semantics of the original are replaced with the
// SSD1306 control-byte protocol decoder used by the pico2 examples:
// the first byte after START is a control byte (bit6 = D/C#, bit7 = Co),
// following bytes are pushed to the ring buffer as [D/C << 8 | byte].
//
// The ESP32-P4 slave supports hardware clock stretching: SCL is held low
// on address match / RX-full until the ISR services the FIFO, so no data
// is lost regardless of ISR latency.

#include "lcdtap/m5tab5/i2c_slave.hpp"

#include <sdkconfig.h>

#include <esp_idf_version.h>
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
#error "This module requires ESP-IDF v5.5+ (Arduino-ESP32 3.x)."
#endif

#include <soc/soc_caps.h>
#if !defined(SOC_I2C_SUPPORT_SLAVE) || !SOC_I2C_SUPPORT_SLAVE
#error "This chip has no I2C slave support."
#endif
#if !defined(SOC_I2C_SLAVE_CAN_GET_STRETCH_CAUSE) || \
    !SOC_I2C_SLAVE_CAN_GET_STRETCH_CAUSE
#error "This module assumes a stretch-capable I2C slave (ESP32-P4)."
#endif

#include <driver/gpio.h>
#include <esp_intr_alloc.h>
#include <esp_private/periph_ctrl.h>  // PERIPH_RCC_ATOMIC
#include <esp_rom_gpio.h>
#include <hal/i2c_ll.h>
#include <hal/i2c_types.h>
#include <soc/i2c_periph.h>

namespace lcdtap::m5tab5 {

static I2cSlaveState *sI2c = nullptr;
static i2c_dev_t *sDev = nullptr;
static intr_handle_t sIntr = nullptr;
static volatile bool sInitDone = false;
static esp_err_t sInitResult = ESP_FAIL;

static constexpr uint32_t INT_RX_WM = I2C_RXFIFO_WM_INT_ENA_M;
static constexpr uint32_t INT_TX_WM = I2C_TXFIFO_WM_INT_ENA_M;
static constexpr uint32_t INT_TRANS_COMPLETE = I2C_TRANS_COMPLETE_INT_ENA_M;

//=============================================================================
// ISR internals
//=============================================================================

// Decode received bytes through the SSD1306 control-byte state machine and
// push payload bytes into the ring buffer.
static void IRAM_ATTR drainRxFifo(I2cSlaveState *s, uint32_t count) {
  uint8_t buf[SOC_I2C_FIFO_LEN];
  while (count) {
    uint8_t c = (count > SOC_I2C_FIFO_LEN) ? SOC_I2C_FIFO_LEN : (uint8_t)count;
    i2c_ll_read_rxfifo(sDev, buf, c);
    for (uint8_t i = 0; i < c; ++i) {
      uint8_t byte = buf[i];
      if (s->rxState == I2cRxState::WAIT_CTRL) {
        // SSD1306 control byte: bit6 = D/C#, bit7 = Co.
        // Co=1: exactly one payload byte follows, then another control byte.
        bool dc = (byte >> 6u) & 1u;
        s->coSingleByte = (byte >> 7u) & 1u;
        s->rxState = dc ? I2cRxState::STREAM_DATA : I2cRxState::STREAM_CMD;
      } else {
        uint32_t word = (s->rxState == I2cRxState::STREAM_DATA)
                            ? (0x100u | byte)
                            : static_cast<uint32_t>(byte);
        s->ringBuf[s->writeIdx] = word;
        s->writeIdx = (s->writeIdx + 1u) & (s->ringWords - 1u);
        if (s->coSingleByte) {
          s->rxState = I2cRxState::WAIT_CTRL;
        }
      }
    }
    count -= c;
  }
}

// Respond to reads with dummy bytes (read-back not supported).
static void IRAM_ATTR fillTxFifoDummy(void) {
  uint32_t space = 0;
  i2c_ll_get_txfifo_len(sDev, &space);
  if (space == 0) return;
  if (space > SOC_I2C_FIFO_LEN) space = SOC_I2C_FIFO_LEN;
  uint8_t buf[SOC_I2C_FIFO_LEN];
  for (uint32_t i = 0; i < space; ++i) buf[i] = 0xFFu;
  i2c_ll_write_txfifo(sDev, buf, (uint8_t)space);
}

static void IRAM_ATTR i2cIsrHandler(void *) {
  I2cSlaveState *s = sI2c;
  i2c_dev_t *dev = sDev;

  uint32_t ints = 0;
  i2c_ll_get_intr_mask(dev, &ints);
  if (ints == 0) return;
  i2c_ll_clear_intr_mask(dev, ints);

  uint32_t rxCount = 0;
  i2c_ll_get_rxfifo_cnt(dev, &rxCount);
  const bool isRead =
      (i2c_ll_slave_get_read_write_status(dev) == I2C_SLAVE_READ_BY_MASTER);

  bool notify = false;

  // --- (1) Drain received data ---------------------------------------------
  if (rxCount) {
    drainRxFifo(s, rxCount);
    notify = true;
  }

  // --- (2) Keep long reads flowing / stop stale watermark interrupts --------
  if (ints & INT_TX_WM) {
    if (isRead) {
      fillTxFifoDummy();
    } else {
      i2c_ll_disable_intr_mask(dev, INT_TX_WM);
    }
  }

  // --- (3) STOP (transaction complete) ---------------------------------------
  // Unconditional TX cleanup: see the reference example for why this must not
  // be guarded by isRead (a pending address match may have overwritten it).
  if (ints & INT_TRANS_COMPLETE) {
    i2c_ll_disable_intr_mask(dev, INT_TX_WM);
    i2c_ll_txfifo_rst(dev);
    // Next transaction starts with a fresh control byte.
    s->rxState = I2cRxState::WAIT_CTRL;
    s->coSingleByte = false;
  }

  // --- (4) Clock stretch handling --------------------------------------------
  if (ints & I2C_SLAVE_STRETCH_INT_ENA_M) {
    i2c_slave_stretch_cause_t cause;
    i2c_ll_slave_get_stretch_cause(dev, &cause);
    switch (cause) {
      case I2C_SLAVE_STRETCH_CAUSE_ADDRESS_MATCH:
        if (isRead) {
          i2c_ll_txfifo_rst(dev);
          fillTxFifoDummy();
          i2c_ll_enable_intr_mask(dev, INT_TX_WM);
        }
        i2c_ll_slave_clear_stretch(dev);
        break;

      case I2C_SLAVE_STRETCH_CAUSE_TX_EMPTY:
        fillTxFifoDummy();
        i2c_ll_slave_clear_stretch(dev);
        break;

      case I2C_SLAVE_STRETCH_CAUSE_RX_FULL:
        i2c_ll_get_rxfifo_cnt(dev, &rxCount);
        if (rxCount) {
          drainRxFifo(s, rxCount);
          notify = true;
        }
        i2c_ll_slave_clear_stretch(dev);
        break;

      default: i2c_ll_slave_clear_stretch(dev); break;
    }
  }

  // --- (5) Wake the drain task -----------------------------------------------
  if (notify && s->drainTask != nullptr) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s->drainTask, &woken);
    if (woken != pdFALSE) portYIELD_FROM_ISR();
  }
}

//=============================================================================
// Init / deinit
//=============================================================================

static esp_err_t initPeripheral(I2cSlaveState *s) {
  const int port = s->cfg.port;

  // (a) Bus clock + register reset, then controller (function) clock.
  // P4 has SOC_PERIPH_CLK_CTRL_SHARED, so both must run inside
  // PERIPH_RCC_ATOMIC blocks.
  PERIPH_RCC_ATOMIC() {
    i2c_ll_enable_bus_clock(port, true);
    i2c_ll_reset_register(port);
  }

  i2c_dev_t *dev = I2C_LL_GET_HW(port);
  sDev = dev;

  PERIPH_RCC_ATOMIC() {
    i2c_ll_enable_controller_clock(dev, true);
    i2c_ll_set_source_clk(dev, I2C_CLK_SRC_DEFAULT);
  }

  // (b) Wire SDA/SCL manually through the GPIO matrix as open-drain
  // input/output so the slave can hold SCL low (clock stretching).
  {
    const gpio_num_t pins[2] = {(gpio_num_t)s->cfg.pinSda,
                                (gpio_num_t)s->cfg.pinScl};
    const uint32_t outSig[2] = {i2c_periph_signal[port].sda_out_sig,
                                i2c_periph_signal[port].scl_out_sig};
    const uint32_t inSig[2] = {i2c_periph_signal[port].sda_in_sig,
                               i2c_periph_signal[port].scl_in_sig};
    for (int i = 0; i < 2; ++i) {
      esp_rom_gpio_pad_select_gpio(pins[i]);
      esp_err_t err = gpio_set_level(pins[i], 1);
      if (err == ESP_OK) {
        err = gpio_set_direction(pins[i], GPIO_MODE_INPUT_OUTPUT_OD);
      }
      // Internal pull-up (~45k) is a safety net only; external pull-ups
      // (2.2k-10k) are required for reliable operation.
      if (err == ESP_OK) err = gpio_set_pull_mode(pins[i], GPIO_PULLUP_ONLY);
      if (err != ESP_OK) return err;
      esp_rom_gpio_connect_out_signal(pins[i], outSig[i], false, false);
      esp_rom_gpio_connect_in_signal(pins[i], inSig[i], false);
    }
  }

  // (c) Quiesce interrupts and FIFOs before configuring.
  i2c_ll_disable_intr_mask(dev, I2C_LL_INTR_MASK);
  i2c_ll_clear_intr_mask(dev, I2C_LL_INTR_MASK);
  i2c_ll_txfifo_rst(dev);
  i2c_ll_rxfifo_rst(dev);

  // (d) Slave mode, open-drain outputs, TX auto-start on read address match.
  dev->ctr.ms_mode = 0;
  dev->ctr.sda_force_out = 1;
  dev->ctr.scl_force_out = 1;
  i2c_ll_slave_enable_auto_start(dev, true);

  i2c_ll_set_slave_addr(dev, s->cfg.slaveAddr, false);
  i2c_ll_set_tout(dev, I2C_LL_MAX_TIMEOUT);
  i2c_ll_set_sda_timing(dev, 10, 10);
  i2c_ll_master_set_filter(dev, 7);

  // (e) FIFO watermarks at half depth (~16-byte ISR cadence).
  i2c_ll_set_rxfifo_full_thr(dev, SOC_I2C_FIFO_LEN / 2);
  i2c_ll_set_txfifo_empty_thr(dev, SOC_I2C_FIFO_LEN / 2);
  i2c_ll_enable_fifo_mode(dev, true);
  dev->fifo_conf.fifo_prt_en = 1;
  dev->fifo_conf.fifo_addr_cfg_en = 0;

  // (f) Clock stretching.
  i2c_ll_slave_enable_scl_stretch(dev, true);
  i2c_ll_slave_set_stretch_protect_num(dev, 0x3ff);
  i2c_ll_slave_clear_stretch(dev);

  // (g) IRAM ISR; runs on the core that called esp_intr_alloc (core 0 via
  // the setup task below).
  esp_err_t err = esp_intr_alloc(i2c_periph_signal[port].irq,
                                 ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3,
                                 i2cIsrHandler, nullptr, &sIntr);
  if (err != ESP_OK) return err;

  // (h) Enable only the interrupts we use. INT_TX_WM is enabled dynamically
  // when a read transaction starts.
  i2c_ll_enable_intr_mask(
      dev, INT_RX_WM | INT_TRANS_COMPLETE | I2C_SLAVE_STRETCH_INT_ENA_M);
  i2c_ll_update(dev);

  return ESP_OK;
}

// The ISR runs on the core that executed esp_intr_alloc, so initialization
// is done from a task pinned to core 0.
static void setupTask(void *arg) {
  sInitResult = initPeripheral(static_cast<I2cSlaveState *>(arg));
  sInitDone = true;
  vTaskDelete(nullptr);
}

esp_err_t i2cSlaveInit(I2cSlaveState *s, const I2cSlaveConfig &cfg,
                       uint32_t *ringBuf, uint32_t ringWords) {
  sI2c = s;
  s->cfg = cfg;
  s->ringBuf = ringBuf;
  s->ringWords = ringWords;
  s->writeIdx = 0;
  s->readIdx = 0;
  s->rxState = I2cRxState::WAIT_CTRL;
  s->coSingleByte = false;

  sInitDone = false;
  sInitResult = ESP_FAIL;
  if (xTaskCreatePinnedToCore(setupTask, "i2c_slave_init", 4096, s, 10, nullptr,
                              0) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  while (!sInitDone) vTaskDelay(1);
  s->active = (sInitResult == ESP_OK);
  return sInitResult;
}

void i2cSlaveDeinit(I2cSlaveState *s) {
  if (!s->active) return;
  if (sIntr) {
    esp_intr_free(sIntr);
    sIntr = nullptr;
  }
  if (sDev) {
    i2c_ll_disable_intr_mask(sDev, I2C_LL_INTR_MASK);
    PERIPH_RCC_ATOMIC() { i2c_ll_enable_bus_clock(s->cfg.port, false); }
    sDev = nullptr;
  }
  s->readIdx = 0;
  s->writeIdx = 0;
  s->rxState = I2cRxState::WAIT_CTRL;
  s->active = false;
}

void i2cSlaveProcess(I2cSlaveState *s) {
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

}  // namespace lcdtap::m5tab5
