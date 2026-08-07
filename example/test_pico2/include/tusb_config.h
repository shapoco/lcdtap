#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

// Dual-role TinyUSB configuration (pattern proven in mimicusb):
// - Device stack on the native USB controller (rhport 0) faces the PC:
//   CDC for the debug console (pico_stdio_usb in app-owned-TinyUSB mode),
//   plus MSC in Phase 2 (BOOTSEL passthrough).
// - Host stack on the PIO-USB controller (rhport 1) faces the target:
//   CDC for the JSON protocol, MSC for download-mode detection/passthrough.

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_OS OPT_OS_PICO

#define CFG_TUD_ENABLED 1
#define BOARD_TUD_RHPORT 0

#define CFG_TUH_ENABLED 1
#define CFG_TUH_RPI_PIO_USB 1
#define BOARD_TUH_RHPORT 1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE (PC-facing)
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

#define CFG_TUD_CDC 1

// Phase 2: MSC passthrough of the target's BOOTSEL drive.
#ifndef TESTRIG_MSC_PASSTHROUGH
#define TESTRIG_MSC_PASSTHROUGH 0
#endif
#define CFG_TUD_MSC (TESTRIG_MSC_PASSTHROUGH)

#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 512
#define CFG_TUD_CDC_EP_BUFSIZE 64
#define CFG_TUD_MSC_EP_BUFSIZE 512

//--------------------------------------------------------------------
// HOST (target-facing)
//--------------------------------------------------------------------

#define CFG_TUH_ENUMERATION_BUFSIZE 512

#define CFG_TUH_HUB 0
#define CFG_TUH_CDC 1
#define CFG_TUH_MSC 1
#define CFG_TUH_HID 0
#define CFG_TUH_VENDOR 0

#define CFG_TUH_DEVICE_MAX 1

// Assert DTR/RTS on enumeration: the target's pico_stdio_usb blocks output
// until DTR is seen.
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM \
  (CDC_CONTROL_LINE_STATE_DTR | CDC_CONTROL_LINE_STATE_RTS)

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
