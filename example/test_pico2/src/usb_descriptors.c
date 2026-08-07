// USB descriptors for the device (PC-facing) side of the test rig.
// CDC debug console; Phase 2 adds the MSC passthrough interface.
// Ported from mimicusb firmware/src/usb_descriptors.c.

#include "pico/unique_id.h"
#include "tusb.h"

#define USB_VID 0xCafe
#define USB_BCD 0x0200

// Auto ProductID from enabled classes so the PC caches a driver set that
// matches this exact interface combination.
#define PID_MAP(itf, n) ((CFG_TUD_##itf) ? (1 << (n)) : 0)
#define USB_PID (0x4100 | PID_MAP(CDC, 0) | PID_MAP(MSC, 1))

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
static tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,

    // IAD is required to group the two CDC interfaces.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,

    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
enum {
  ITF_NUM_CDC = 0,
  ITF_NUM_CDC_DATA,
#if CFG_TUD_MSC
  ITF_NUM_MSC,
#endif
  ITF_NUM_TOTAL,
};

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82
#define EPNUM_MSC_OUT 0x03
#define EPNUM_MSC_IN 0x83

#if CFG_TUD_MSC
#define CONFIG_TOTAL_LEN \
  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)
#else
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#endif

static uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 16, EPNUM_CDC_OUT,
                       EPNUM_CDC_IN, 64),
#if CFG_TUD_MSC
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
#endif
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
};

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},  // 0: English (0x0409)
    "shapoco",                   // 1: Manufacturer
    "LcdTap TestRig",            // 2: Product
    NULL,                        // 3: Serial, filled from board unique id
    "TestRig CDC",               // 4: CDC Interface
    "TestRig MSC",               // 5: MSC Interface
};

static uint16_t _desc_str[32 + 1];

static size_t board_serial(uint16_t *utf16, size_t max_chars) {
  pico_unique_board_id_t uid;
  pico_get_unique_board_id(&uid);
  size_t chr_count = 0;
  for (size_t i = 0;
       i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES && chr_count + 2 <= max_chars; i++) {
    static const char hex[] = "0123456789ABCDEF";
    utf16[chr_count++] = hex[(uid.id[i] >> 4) & 0xF];
    utf16[chr_count++] = hex[uid.id[i] & 0xF];
  }
  return chr_count;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  size_t chr_count;

  switch (index) {
    case STRID_LANGID:
      memcpy(&_desc_str[1], string_desc_arr[0], 2);
      chr_count = 1;
      break;

    case STRID_SERIAL: chr_count = board_serial(&_desc_str[1], 32); break;

    default: {
      if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
        return NULL;
      }
      const char *str = string_desc_arr[index];
      chr_count = strlen(str);
      if (chr_count > 32) chr_count = 32;
      for (size_t i = 0; i < chr_count; i++) {
        _desc_str[1 + i] = str[i];
      }
      break;
    }
  }

  _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
  return _desc_str;
}
