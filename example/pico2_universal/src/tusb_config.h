#pragma once

// TinyUSB configuration — MSC device only (no CDC, HID, MIDI, etc.)

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE

#define CFG_TUD_MSC 1
#define CFG_TUD_MSC_EP_BUFSIZE 512

#define CFG_TUD_CDC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_AUDIO 0
#define CFG_TUD_VIDEO 0
#define CFG_TUD_VENDOR 0
