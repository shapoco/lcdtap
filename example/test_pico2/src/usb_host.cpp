#include "testrig/usb_host.hpp"

#include "hardware/dma.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pio_usb.h"
#include "testrig/config.h"
#include "testrig/spsc_ring.hpp"
#include "tusb.h"

namespace testrig {

namespace {

constexpr uint8_t RHPORT_HOST = 1;

// Cross-core byte streams. Each ring has exactly one producer core and one
// consumer core.
SpscRing<2048> gRingToTarget;    // produced on core0, consumed on core1
SpscRing<8192> gRingFromTarget;  // produced on core1, consumed on core0

volatile int8_t gCdcIdx = -1;  // mounted target CDC interface, -1 = none
volatile bool gMscMounted = false;

// Set once the host stack owns its pio0/pio1 resources; core 0 gates the
// CYW43 init on this (see main.cpp).
volatile bool gHostReady = false;

// Explicit core 1 stack: the SDK default is 2 KB, which is tight for the
// TinyUSB host stack plus the PIO-USB IRQs.
uint32_t gCore1Stack[4096 / sizeof(uint32_t)];

// core1: pump the rings against the host CDC FIFOs.
void cdcHostTask() {
  int8_t idx = gCdcIdx;
  if (idx < 0) {
    // No target: discard queued bytes so a later mount starts clean.
    gRingToTarget.drain();
    return;
  }

  uint8_t buf[64];

  while (!gRingToTarget.empty()) {
    uint32_t writable = tuh_cdc_write_available((uint8_t)idx);
    if (writable == 0) break;
    uint32_t chunk = writable < sizeof(buf) ? writable : sizeof(buf);
    uint32_t n = gRingToTarget.pop(buf, chunk);
    if (n == 0) break;
    tuh_cdc_write((uint8_t)idx, buf, n);
  }
  tuh_cdc_write_flush((uint8_t)idx);

  while (tuh_cdc_read_available((uint8_t)idx)) {
    uint32_t n = tuh_cdc_read((uint8_t)idx, buf, sizeof(buf));
    if (n == 0) break;
    // If the ring is full the excess is dropped; the JSON layer recovers by
    // timeout + retry, and correctly sized rings make this a non-event.
    gRingFromTarget.push(buf, n);
  }
}

void core1Main() {
  sleep_ms(10);

  pio_usb_configuration_t pioCfg = PIO_USB_DEFAULT_CONFIG;
  pioCfg.pin_dp = PIN_USB_DP;  // D- is implicitly pin_dp + 1
  // PIO-USB claims the FIXED DMA channel from the config (default 0) via
  // dma_claim_mask(), but busInit() has already taken channel 0 -> panic
  // "DMA channel 0 is already claimed". Reserve a genuinely free channel
  // and hand over its number (pio_usb claims it itself, so unclaim first).
  int txCh = dma_claim_unused_channel(true);
  dma_channel_unclaim(static_cast<uint>(txCh));
  pioCfg.tx_ch = static_cast<uint8_t>(txCh);
  tuh_configure(RHPORT_HOST, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pioCfg);

  tuh_init(RHPORT_HOST);
  gHostReady = true;

  while (true) {
    tuh_task();
    cdcHostTask();
  }
}

}  // namespace

void usbHostStart() {
  multicore_reset_core1();
  multicore_launch_core1_with_stack(core1Main, gCore1Stack,
                                    sizeof(gCore1Stack));
}

bool usbHostReady() { return gHostReady; }

bool usbHostCdcMounted() { return gCdcIdx >= 0; }

bool usbHostMscMounted() { return gMscMounted; }

uint32_t usbHostCdcWrite(const uint8_t* data, uint32_t len) {
  return gRingToTarget.push(data, len);
}

uint32_t usbHostCdcRead(uint8_t* data, uint32_t len) {
  return gRingFromTarget.pop(data, len);
}

void usbHostCdcDrainInput() { gRingFromTarget.drain(); }

}  // namespace testrig

//--------------------------------------------------------------------+
// TinyUSB host callbacks (core 1)
//--------------------------------------------------------------------+

extern "C" void tuh_cdc_mount_cb(uint8_t idx) {
  testrig::gCdcIdx = (int8_t)idx;
}

extern "C" void tuh_cdc_umount_cb(uint8_t idx) {
  if (testrig::gCdcIdx == (int8_t)idx) testrig::gCdcIdx = -1;
}

// Target entered BOOTSEL (RPI-RP2 drive). Phase 1 only reports it; Phase 2
// bridges it to the PC.
extern "C" void tuh_msc_mount_cb(uint8_t devAddr) {
  (void)devAddr;
  testrig::gMscMounted = true;
}

extern "C" void tuh_msc_umount_cb(uint8_t devAddr) {
  (void)devAddr;
  testrig::gMscMounted = false;
}
