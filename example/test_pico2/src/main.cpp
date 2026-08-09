// LcdTap test rig (Pico 2 W) — entry point.
//
// Core 0: PC-facing USB device (CDC console), OLED UI, inputs, test
// executor. Core 1: PIO-USB host stack facing the target (usb_host.cpp).

#include <cstdio>

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"
#include "pico/cyw43_arch.h"
#include "pico/cyw43_driver.h"
#include "pico/stdlib.h"
#include "testrig/bus.hpp"
#include "testrig/config.h"
#include "testrig/config_cmds.hpp"
#include "testrig/input.hpp"
#include "testrig/jsonclient.hpp"
#include "testrig/link.hpp"
#include "testrig/oled.hpp"
#include "testrig/ui.hpp"
#include "testrig/rig_config.hpp"
#include "testrig/usb_device.hpp"
#include "testrig/usb_host.hpp"
#include "testrig/wifi_mgr.hpp"
#include "tusb.h"

#if TESTRIG_CYW43_DIAG
// ---------------------------------------------------------------------------
// CYW43 fault isolation: bring the chip up and back down under progressively
// more of the rig's runtime conditions. The first stage whose rc turns
// nonzero names the culprit. PIO clock is pinned near the stock ratio
// (~75 MHz) at every stage via the dynamic divider.
// ---------------------------------------------------------------------------
static int diagTry() {
  int rc = cyw43_arch_init();
  if (rc != 0) return rc | 0x1000;
  rc = cyw43_gpio_set(&cyw43_state, CYW43_WL_GPIO_LED_PIN, false);
  cyw43_arch_deinit();
  return rc;
}

int main() {
  int rcA, rcB, rcC, rcD, rcE;

  // A: near-stock (default 150 MHz clock, nothing else initialized)
  cyw43_set_pio_clock_divisor(2, 0);  // 75 MHz PIO
  rcA = diagTry();

  // B: + vreg bump and 240 MHz clk_sys
  vreg_set_voltage(VREG_VOLTAGE_1_15);
  sleep_ms(1);
  set_sys_clock_khz(SYS_CLOCK_KHZ, true);
  cyw43_set_pio_clock_divisor(3, 64);  // ~73.8 MHz PIO at 240 MHz
  rcB = diagTry();

  // C: + USB device stack (CDC console)
  tud_init(0);
  testrig::usbDeviceStdioInit();

  testrig::rigConfigLoad();
  rcC = diagTry();

  // D: + OLED / inputs / bus master init
  testrig::oledInit();
  testrig::inputInit();
  testrig::busInit();
  rcD = diagTry();

  // E: + PIO-USB host on core 1, init inside the pio0/pio1 claim window
  testrig::usbHostStart();
  absolute_time_t hostDeadline = make_timeout_time_ms(1000);
  while (!testrig::usbHostReady() &&
         absolute_time_diff_us(get_absolute_time(), hostDeadline) > 0) {
    sleep_ms(10);
  }
  uint32_t dclaimed[2] = {0, 0};
  PIO dpios[2] = {pio0, pio1};
  for (int p = 0; p < 2; p++) {
    for (uint sm = 0; sm < 4; sm++) {
      if (!pio_sm_is_claimed(dpios[p], sm)) {
        pio_sm_claim(dpios[p], sm);
        dclaimed[p] |= 1u << sm;
      }
    }
  }
  rcE = cyw43_arch_init();
  if (rcE == 0) {
    rcE = cyw43_gpio_set(&cyw43_state, CYW43_WL_GPIO_LED_PIN, false);
  } else {
    rcE |= 0x1000;
  }
  for (int p = 0; p < 2; p++) {
    for (uint sm = 0; sm < 4; sm++) {
      if (dclaimed[p] & (1u << sm)) pio_sm_unclaim(dpios[p], sm);
    }
  }

  char line[22];
  testrig::oledClear();
  testrig::oledText(0, 0, "CYW43 diag");
  snprintf(line, sizeof(line), "A stock   %d", rcA);
  testrig::oledText(0, 1, line);
  snprintf(line, sizeof(line), "B 240MHz  %d", rcB);
  testrig::oledText(0, 2, line);
  snprintf(line, sizeof(line), "C +tud    %d", rcC);
  testrig::oledText(0, 3, line);
  snprintf(line, sizeof(line), "D +periph %d", rcD);
  testrig::oledText(0, 4, line);
  snprintf(line, sizeof(line), "E +piousb %d", rcE);
  testrig::oledText(0, 5, line);
  testrig::oledFlush();

  bool ledOn = false;
  while (true) {
    tud_task();
    printf("CYW43 diag: A=%d B=%d C=%d D=%d E=%d\n", rcA, rcB, rcC, rcD,
           rcE);
    if (rcE == 0) {
      ledOn = !ledOn;
      cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, ledOn);
    }
    sleep_ms(2000);
  }
}

#else

int main() {
  // 240 MHz (PIO-USB compatible multiple of 12 MHz) needs a small core
  // voltage bump on RP2350. Set the clock before any USB init.
  vreg_set_voltage(VREG_VOLTAGE_1_15);
  sleep_ms(1);
  set_sys_clock_khz(SYS_CLOCK_KHZ, true);

  tud_init(0);
  testrig::usbDeviceStdioInit();

  testrig::rigConfigLoad();

  testrig::oledInit();
  testrig::inputInit();
  testrig::busInit();

  testrig::usbHostStart();

  // Wait for the host stack to claim its pio0/pio1 resources, then steer
  // the CYW43 LED driver's free-SM search away from them: temporarily claim
  // every free SM on pio0/pio1 so the driver lands on pio2 (SM0 there is
  // reserved by busInit).
  absolute_time_t deadline = make_timeout_time_ms(1000);
  while (!testrig::usbHostReady() &&
         absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
    sleep_ms(10);
  }
  uint32_t claimed[2] = {0, 0};
  PIO pios[2] = {pio0, pio1};
  for (int p = 0; p < 2; p++) {
    for (uint sm = 0; sm < 4; sm++) {
      if (!pio_sm_is_claimed(pios[p], sm)) {
        pio_sm_claim(pios[p], sm);
        claimed[p] |= 1u << sm;
      }
    }
  }
  // The CYW43 PIO SPI must run near the stock PIO clock (~75 MHz = 150 MHz
  // / div 2): the sample-timing-tuned SPI program fails at 60 MHz and below
  // (verified on hardware via the staged diag build). Derive the divider
  // from clk_sys instead of hardcoding it.
  uint64_t div256 =
      (static_cast<uint64_t>(clock_get_hz(clk_sys)) << 8) / 75000000u;
  cyw43_set_pio_clock_divisor(static_cast<uint16_t>(div256 >> 8),
                              static_cast<uint8_t>(div256 & 0xFFu));

  // The CYW43 chip (and its PIO SPI claim) initializes lazily on the first
  // GPIO access, not in cyw43_arch_init. Bring it up here, with a bounded
  // retry: every failed attempt leaks one PIO SM inside the driver (no
  // deinit on its failure path), so retrying forever would exhaust pio2 and
  // spam the console.
  int rcArch = cyw43_arch_init();
  int rcUp = -1000;
  if (rcArch == 0) {
    for (int i = 0; i < 3 && rcUp != 0; i++) {
      rcUp = cyw43_gpio_set(&cyw43_state, CYW43_WL_GPIO_LED_PIN, false);
      if (rcUp != 0) sleep_ms(300);
    }
  }
  bool ledOk = (rcArch == 0 && rcUp == 0);
  for (int p = 0; p < 2; p++) {
    for (uint sm = 0; sm < 4; sm++) {
      if (claimed[p] & (1u << sm)) pio_sm_unclaim(pios[p], sm);
    }
  }

  static testrig::CdcTransport transport;
  static testrig::JsonClient client(&transport);
  testrig::uiInit(&client, &transport);

  absolute_time_t nextBlink = get_absolute_time();
  absolute_time_t nextReport = get_absolute_time();
  bool ledOn = false;

  while (true) {
    tud_task();
    cyw43_arch_poll();
    testrig::wifiMgrProcess(to_ms_since_boot(get_absolute_time()));
    testrig::configCmdsProcess();
    testrig::uiTick();

    if (absolute_time_diff_us(get_absolute_time(), nextBlink) <= 0) {
      nextBlink = make_timeout_time_ms(500);
      ledOn = !ledOn;
      if (ledOk) cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, ledOn);
    }
    if (!ledOk &&
        absolute_time_diff_us(get_absolute_time(), nextReport) <= 0) {
      nextReport = make_timeout_time_ms(10000);
      printf("CYW43 down: arch=%d up=%d (LED disabled)\n", rcArch, rcUp);
    }
  }
}

#endif  // TESTRIG_CYW43_DIAG
