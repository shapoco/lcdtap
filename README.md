# SpiLcd2Dvi

A library and example that receives ST7789 SPI LCD commands as an SPI slave
and outputs the framebuffer as a DVI-D signal.

## Library: SpiLcd2Dvi

Include the library header:

```cpp
#include <spilcd2dvi/spilcd2dvi.hpp>
```

All symbols live in the `sl2d` namespace.

### `Sl2dConfig` — configuration

```cpp
struct Sl2dConfig {
  // SPI input (LCD) side
  uint16_t lcdWidth;          // LCD horizontal resolution
  uint16_t lcdHeight;         // LCD vertical resolution
  PixelFormat pixelFormat;    // RGB444 / RGB565 / RGB666 (match ST7789 COLMOD)

  // DVI output side
  uint16_t dviWidth;          // DVI active-area width in pixels
  uint16_t dviHeight;         // DVI active-area height in lines

  ScaleMode scaleMode;        // STRETCH / FIT / PIXEL_PERFECT

  bool invertInvPolarity;     // true: INVON → normal, INVOFF → inverted
                              // false (default): INVON → inverted (ST7789 spec)
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

### `HostInterface` — platform callbacks

```cpp
struct HostInterface {
  void* (*alloc)(size_t size);              // required: allocate memory
  void  (*free)(void* ptr);                 // required: free memory
  void  (*log)(void* userData,
               const char* message);        // optional: debug log (nullptr = off)
  void* userData;                           // passed back to log()
};
```

The library never calls `malloc` internally; all allocations go through
`alloc`/`free`.  On MCUs with scratchpad SRAM, point `alloc` to a bump
allocator over that region for best performance.

Use `calcRequiredMemory(config)` to determine the total bytes needed before
constructing the instance.

### `SpiLcd2Dvi` — main class

```cpp
class SpiLcd2Dvi {
public:
  SpiLcd2Dvi(const Sl2dConfig& config, const HostInterface& host);
  ~SpiLcd2Dvi();

  Status getStatus() const;           // check constructor result

  // SPI input
  void inputReset(bool assert);       // drive RESX (true = assert reset)
  void inputCommand(uint8_t byte);    // feed one command byte  (DCX = low)
  void inputData(const uint8_t* data, size_t length); // feed data bytes (DCX = high)

  // DVI output
  void fillScanline(uint16_t line, uint16_t* dst) const;
    // Writes one DVI scanline (RGB565, dviWidth pixels) to dst.
    // Scaling, letterboxing, and colour inversion are applied internally.
    // Call this for every line 0 .. dviHeight-1 each frame.

  // Debug / test
  uint16_t* getFramebuf();            // direct access to the internal framebuffer
  void setDisplayOn(bool on);         // force display on/off before SLPOUT+DISPON
};
```

`Status` values: `OK`, `INVALID_PARAM`, `OUT_OF_MEMORY`, `NOT_READY`.

### Typical usage

```cpp
// 1. Configure
sl2d::Sl2dConfig cfg;
cfg.lcdWidth         = 240;
cfg.lcdHeight        = 320;
cfg.pixelFormat      = sl2d::PixelFormat::RGB565;
cfg.dviWidth         = 640;
cfg.dviHeight        = 480;
cfg.scaleMode        = sl2d::ScaleMode::FIT;
cfg.invertInvPolarity = false;

// 2. Provide platform callbacks
sl2d::HostInterface host;
host.alloc    = myAlloc;   // e.g. bump allocator over SRAM
host.free     = myFree;
host.log      = nullptr;
host.userData = nullptr;

// 3. Construct (allocates framebuffer via host.alloc)
sl2d::SpiLcd2Dvi inst(cfg, host);
if (inst.getStatus() != sl2d::Status::OK) { /* handle error */ }

// 4. Feed incoming SPI bytes (call from SPI interrupt / DMA handler)
inst.inputReset(false);                   // RESX de-asserted
inst.inputCommand(0x29);                  // DISPON
inst.inputData(pixelData, byteCount);     // RAMWR payload

// 5. Output DVI scanlines (call from DVI scanline loop)
for (uint16_t y = 0; y < 480; ++y) {
    inst.fillScanline(y, scanlineBuf);
    // hand scanlineBuf to the DVI output layer
}
```

## Example: Raspberry Pi Pico2

See [example/pico2/README.md](example/pico2/README.md) for build instructions,
pin assignment, and configuration details.

## License

MIT License — see [LICENSE](LICENSE).
