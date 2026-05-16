# LcdTap

A library and example that receives LCD controller commands (via SPI or I2C)
and outputs the framebuffer as a DVI-D signal.

## Example: Raspberry Pi Pico2

### for ST7789

See [example/pico2_st7789/README.md](example/pico2_st7789/README.md) for build
instructions, pin assignment, and configuration details.

### for SSD1306

See [example/pico2_ssd1306/README.md](example/pico2_ssd1306/README.md) for build
instructions, pin assignment, and configuration details.

## Download Binary

See [releases](https://github.com/shapoco/lcdtap/releases) for pre-built UF2 binaries.

## Video

https://github.com/user-attachments/assets/6f17d5dc-84d3-4a2a-a3ea-fca37591515f

## Library usage

Include the library header:

```cpp
#include <lcdtap/lcdtap.hpp>
```

All symbols live in the `lcdtap` namespace.

### `ControllerType` — LCD controller type

```cpp
enum class ControllerType : uint8_t {
  ST7789,
  SSD1306,
};
```

### `LcdTapConfig` — configuration

```cpp
struct LcdTapConfig {
  // LCD controller
  ControllerType controller;

  // SPI input (LCD) side
  uint16_t lcdWidth;          // LCD horizontal resolution
  uint16_t lcdHeight;         // LCD vertical resolution
  PixelFormat pixelFormat;    // RGB444 / RGB565 / RGB666 (initial; changed by COLMOD)

  // DVI output side
  uint16_t dviWidth;          // DVI active-area width in pixels
  uint16_t dviHeight;         // DVI active-area height in lines
  ScaleMode scaleMode;        // STRETCH / FIT / PIXEL_PERFECT

  bool invertInvPolarity;     // true: INVON → normal, INVOFF → inverted
                              // false (default): INVON → inverted (ST7789 spec)
  bool swapRB;                // true: swap R and B channels (invert cachedBGR)
                              // false (default): no swap
};
```

`PixelFormat` values:

| Value      | Meaning                     |
|------------|-----------------------------|
| `RGB444`   | 12 bpp (3 bytes → 2 pixels) |
| `RGB565`   | 16 bpp (2 bytes → 1 pixel)  |
| `RGB666`   | 18 bpp (3 bytes → 1 pixel)  |

`ScaleMode` values:

| Value           | Meaning                                                        |
|-----------------|----------------------------------------------------------------|
| `STRETCH`       | Stretch LCD image to fill the entire DVI active area           |
| `FIT`           | Scale preserving aspect ratio; letterbox/pillarbox black bars  |
| `PIXEL_PERFECT` | Integer scale factor; black borders on all sides               |

### `getDefaultConfig` — default configuration

```cpp
void getDefaultConfig(ControllerType type, LcdTapConfig* cfg);
```

Fills `*cfg` with sensible defaults for the specified controller.
Override individual fields as needed before constructing `LcdTap`.

Default values for `ControllerType::ST7789`:

| Field              | Default value        |
|--------------------|----------------------|
| `lcdWidth`         | 240                  |
| `lcdHeight`        | 320                  |
| `pixelFormat`      | `RGB565`             |
| `dviWidth`         | 640                  |
| `dviHeight`        | 480                  |
| `scaleMode`        | `FIT`                |
| `invertInvPolarity`| `false`              |
| `swapRB`           | `false`              |

### `HostInterface` — platform callbacks

```cpp
struct HostInterface {
  void* (*alloc)(size_t size);              // required: allocate framebuffer memory
  void  (*free)(void* ptr);                 // required: free framebuffer memory
  void  (*log)(void* userData,
               const char* message);        // optional: debug log (nullptr = off)
  void* userData;                           // passed back to log()
};
```

`alloc`/`free` are used exclusively for the framebuffer.
On MCUs with external PSRAM, point `alloc` to an allocator over that region
to keep the large framebuffer out of internal SRAM.

### `LcdTap` — main class

```cpp
class LcdTap {
public:
  LcdTap(const LcdTapConfig& config, const HostInterface& host);
  ~LcdTap();

  Status getStatus() const;           // check constructor result

  // SPI/I2C input
  void inputReset(bool assert);       // drive RESX (true = assert reset)
  void inputCommand(uint8_t byte);    // feed one command byte  (DCX = low)
  void inputData(const uint8_t* data, size_t length); // feed data bytes (DCX = high)

  // DVI output
  void fillScanline(uint16_t line, uint16_t* dst) const;
    // Writes one DVI scanline (RGB565, dviWidth pixels) to dst.
    // Scaling, letterboxing, and colour inversion are applied internally.
    // Call this for every line 0 .. dviHeight-1 each frame.

  void setOutputRotation(int rot);
    // Rotate the DVI output image. Default is 0 (no rotation).
    // Does NOT affect controller state or command processing;
    // only the readout pattern of fillScanline() changes.
    // The setting is preserved across inputReset() / softReset().
    //
    // rot=0  no rotation
    // rot=1  90° clockwise   — aspect ratio swapped for FIT / PIXEL_PERFECT
    // rot=2  180° (flip)     — aspect ratio unchanged
    // rot=3  270° clockwise  — aspect ratio swapped for FIT / PIXEL_PERFECT

  // Debug / test
  uint16_t* getFramebuf();            // direct access to the internal framebuffer
  void setDisplayOn(bool on);         // force display on/off before SLPOUT+DISPON
};
```

`Status` values: `OK`, `INVALID_PARAM`, `OUT_OF_MEMORY`, `NOT_READY`.

### Typical usage

```cpp
// 1. Configure — start from controller defaults, then override as needed
lcdtap::LcdTapConfig cfg;
lcdtap::getDefaultConfig(lcdtap::ControllerType::ST7789, &cfg);
cfg.lcdHeight        = 240;               // override for 240×240 variant
cfg.dviWidth         = 640;
cfg.dviHeight        = 480;
cfg.scaleMode        = lcdtap::ScaleMode::FIT;

// 2. Provide platform callbacks (alloc/free used for framebuffer only)
lcdtap::HostInterface host;
host.alloc    = myAlloc;   // e.g. bump allocator over PSRAM
host.free     = myFree;
host.log      = nullptr;
host.userData = nullptr;

// 3. Construct
lcdtap::LcdTap inst(cfg, host);
if (inst.getStatus() != lcdtap::Status::OK) { /* handle error */ }

// 4. (Optional) Rotate the output image 90° clockwise
inst.setOutputRotation(1);

// 5. Feed incoming SPI bytes (call from SPI interrupt / DMA handler)
inst.inputReset(false);                   // RESX de-asserted
inst.inputCommand(0x29);                  // DISPON
inst.inputData(pixelData, byteCount);     // RAMWR payload

// 6. Output DVI scanlines (call from DVI scanline loop)
for (uint16_t y = 0; y < 480; ++y) {
    inst.fillScanline(y, scanlineBuf);
    // hand scanlineBuf to the DVI output layer
}
```

### ST7789 device header

ST7789 command code constants are available in a separate header:

```cpp
#include <lcdtap/devices/st7789.hpp>

// lcdtap::st7789::CMD_NOP, CMD_SWRESET, CMD_SLPOUT, CMD_INVOFF, CMD_INVON,
// CMD_DISPON, CMD_CASET, CMD_RASET, CMD_RAMWR, CMD_MADCTL, CMD_COLMOD
```

## License

MIT License — see [LICENSE](LICENSE).
