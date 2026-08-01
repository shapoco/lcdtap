#pragma once

// 312 MHz system clock setup shared by the pico2 examples.
//
// clk_sys is sourced from PLL_USB (VCO 1248 MHz -> 624 MHz -> /2), leaving
// PLL_SYS entirely free for a video clock (HSTX bit clock) or unused:
//   clk_sys = 312 MHz   clk_peri = 156 MHz
//   clk_usb =  48 MHz   clk_adc  =  48 MHz
// Also raises the core voltage to 1.25 V and re-tightens the QMI flash
// timing for the higher clock.

namespace lcdtap::pico2 {

void sysClockInit312();

// Restore the QMI flash timing for 312 MHz operation. Must be called after
// anything that resets the QMI divisor (e.g. flash_exit_xip via
// flash_range_erase/program). SRAM-resident; must not access flash while the
// slow/default timing question is unsettled, so callers run it from SRAM.
void sysClockSetQmiTiming();

}  // namespace lcdtap::pico2
