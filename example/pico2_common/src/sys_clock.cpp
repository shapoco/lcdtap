#include "lcdtap/pico2/sys_clock.hpp"

#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"

namespace lcdtap::pico2 {

// ---------------------------------------------------------------------------
// QMI flash timing (must not access flash itself)
// ---------------------------------------------------------------------------

void __no_inline_not_in_flash_func(sysClockSetQmiTiming)() {
  while (
      (ioqspi_hw->io[1].status & IO_QSPI_GPIO_QSPI_SS_STATUS_OUTTOPAD_BITS) !=
      IO_QSPI_GPIO_QSPI_SS_STATUS_OUTTOPAD_BITS) {
  }
  qmi_hw->m[0].timing = 0x40000203u;
  volatile uint32_t *xip = (volatile uint32_t *)0x14000000u;
  (void)*xip;
}

// ---------------------------------------------------------------------------
// Clock initialization
// ---------------------------------------------------------------------------

void __no_inline_not_in_flash_func(sysClockInit312)() {
  const uint32_t intr = save_and_disable_interrupts();

  // Slow QMI before touching PLLs to protect flash during clock transitions.
  hw_write_masked(&qmi_hw->m[0].timing, 6, QMI_M0_TIMING_CLKDIV_BITS);

  vreg_set_voltage(VREG_VOLTAGE_1_25);

  // Force a flash read so the slow QMI timing is applied before raising the
  // clock.
  volatile uint32_t *xip = (volatile uint32_t *)0x14000000u;
  (void)*xip;

  // Switch clk_sys and clk_ref off their aux sources before touching PLLs.
  hw_clear_bits(&clocks_hw->clk[clk_sys].ctrl, CLOCKS_CLK_SYS_CTRL_SRC_BITS);
  while (clocks_hw->clk[clk_sys].selected != 0x1u) tight_loop_contents();
  hw_write_masked(&clocks_hw->clk[clk_ref].ctrl,
                  CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC,
                  CLOCKS_CLK_REF_CTRL_SRC_BITS);
  while (clocks_hw->clk[clk_ref].selected != 0x4u) tight_loop_contents();

  clock_stop(clk_usb);
  clock_stop(clk_adc);
  clock_stop(clk_peri);
  clock_stop(clk_hstx);

  constexpr uint32_t USB_PLL_FBDIV = 104;
  constexpr uint32_t USB_VCO_FREQ = 12 * USB_PLL_FBDIV * MHZ;
  constexpr uint32_t USB_PLL_PDIV1 = 2;
  constexpr uint32_t USB_PLL_PDIV2 = 1;
  constexpr uint32_t USB_PLL_FREQ =
      USB_VCO_FREQ / (USB_PLL_PDIV1 * USB_PLL_PDIV2);
  constexpr uint32_t CLK_SYS_FREQ = USB_PLL_FREQ / 2u;
  constexpr uint32_t CLK_PERI_FREQ = USB_PLL_FREQ / 4u;
  constexpr uint32_t CLK_USB_FREQ = 48000u * KHZ;
  constexpr uint32_t CLK_ADC_FREQ = 48000u * KHZ;

  pll_init(pll_usb, PLL_COMMON_REFDIV, USB_VCO_FREQ, USB_PLL_PDIV1,
           USB_PLL_PDIV2);

  clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                  CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, USB_PLL_FREQ,
                  CLK_SYS_FREQ);

  clock_configure(clk_peri, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF,
                  CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                  USB_PLL_FREQ, CLK_PERI_FREQ);

  clock_configure(clk_usb, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF,
                  CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, USB_PLL_FREQ,
                  CLK_USB_FREQ);

  clock_configure(clk_adc, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF,
                  CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, USB_PLL_FREQ,
                  CLK_ADC_FREQ);

  sysClockSetQmiTiming();

  restore_interrupts(intr);
}

}  // namespace lcdtap::pico2
