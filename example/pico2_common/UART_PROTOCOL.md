# UART Protocol

- The host sends a "command" and LcdTap returns a "response".
- Both commands and responses start at the beginning of a line, with content in JSON format, terminated by a newline (CRLF).
- Commands have the following structure. In practice, no newlines or indentation are included.
    
    ```json
    {
        "command": command name (string),
        "params": {
            "paramName1": paramValue1,
            "paramName2": paramValue2,
            "paramName3": paramValue3,
            ...
        }
    }
    ```
    
    - To simplify lexical analysis on the LcdTap side, each token in a command must consist only of ASCII characters and must be at most 64 characters long.
    
- The response is a JSON string corresponding to the command.

## Command Reference

### hello

Verify that LcdTap is connected to the serial port from the host side.

- Command:

    ```json
    {"command": "hello"}
    ```

- Response:
    
    ```json
    {"response": "welcome lcdtap"}
    ```

### getpresets

Get the list of all configuration presets held by LcdTap.

- Command:
    
    ```json
    {"command": "getpresets"}
    ```

- Response:

    ```json
    {"presets": ["ILI9342", "ILI9488", "SSD1306", "SSD1331", "ST7735", "ST7789", "Arduboy", "M5Stack CoreS3", "Thumby", "TinyJoypad", "Xiamocon"]}
    ```

    - The list is derived from the firmware's `CONFIG_PRESET_NAMES` table and may change across firmware versions.

### getparams

Get the list of configuration parameters held by LcdTap as JSON.

- Command:
    
    ```json
    {
        "command": "getparams",
        "preset": "preset name obtained from getpresets (string, optional)"
    }
    ```

    - If `preset` is specified, returns the values for that preset; otherwise returns the current values.

- Response:

    ```json
    {"params":[parameter list]}
    ```

The `id` field of each parameter is `"cfg0"`, `"cfg1"`, ..., `"cfg19"`, corresponding to the `ConfigId` enum index in the firmware. Use the same ids as keys in `setparams`. Clients must treat the ids as opaque keys taken from the `getparams` response; the numbering shifts when `ConfigId` entries are inserted (e.g. `cfg2` became the I2C slave address when it was added after the bus interface).

Two host-side settings appear in the list as well. They are not `ConfigId`s, so they have names instead of indices. **They are not at the end** — they sit immediately before `Output Rotation`, matching the position they occupy in the OSD menu. Match parameters by `id`, never by position:

```json
{
    "id": "outputInterface",
    "type": "ENUM",
    "name": "Output Interface",
    "unit": null,
    "options": {"DVI-D": 0, "NTSC": 1, "PAL": 2, "DisplayLink": 3},
    "value": 0,
    "enableKeyId": "cfg1",
    "enableKeyValueMin": 0,
    "enableKeyValueMax": 2
},
{
    "id": "compositeDac",
    "type": "ENUM",
    "name": "Video DAC Type",
    "unit": null,
    "options": {"PWM": 0, "R-2R": 1},
    "value": 0,
    "enableKeyId": "outputInterface",
    "enableKeyValueMin": 1,
    "enableKeyValueMax": 2
}
```

Note that `compositeDac` is gated on `outputInterface` — an `enableKeyId` can name any parameter, not just a `cfgN`.

Composite output (GPIO5-11) and DisplayLink output (USB D+/D- on GPIO10/11) need GPIOs that the parallel bus already uses, so `outputInterface` is only selectable when `cfg1` (bus interface) is 0-2, and is silently forced back to `DVI-D` otherwise.

### Enable-key cascade

A parameter is enabled when its enable key is in range **and the enable key itself is enabled**. Clients must resolve this transitively, or gating will be wrong wherever the chain is longer than one link.

Concretely: on the parallel bus, `cfg1` is 3, which disables `outputInterface`. `compositeDac` points at `outputInterface`, so it must be disabled too — even though `outputInterface`'s *value* may still read as `NTSC`. Evaluating only the immediate key would leave `compositeDac` selectable there.

`compositeDac` selects which DAC carries the composite signal:

- `PWM` — one GPIO (GPIO10) plus an RC filter. Simple, roughly 12 luma steps and about 70% of the standard amplitude. Works on both SPI and I2C.
- `R-2R` — 7-bit resistor ladder on GPIO5-11 with an emitter follower. Much better picture, but the ladder covers GPIO8/9, so it is **SPI only**. It is silently forced back to `PWM` on I2C or Parallel8.

`compositeDac` is ignored while `outputInterface` is `DVI-D`, but it is still stored, so it takes effect the next time a composite mode is selected.

Parameter list element types:

- Integer:

    ```json
    {
        "id": "cfgN" (string),
        "type": "INTEGER",
        "name": item label (string),
        "unit": unit (string) or null,
        "min": minimum value (integer),
        "max": maximum value (integer),
        "step": step size (integer),
        "value": current value (integer),
        "enableKeyId": "cfgN" of the parameter that gates this item (string, omitted if always enabled),
        "enableKeyValueMin": minimum value of the gate parameter that enables this item (integer, omitted if always enabled),
        "enableKeyValueMax": maximum value of the gate parameter that enables this item (integer, omitted if always enabled)
    }
    ```

- Hex integer (same fields as Integer; the value is a plain integer on the wire, but clients should display it in hexadecimal, e.g. the I2C slave address `0x3E`):

    ```json
    {
        "id": "cfgN" (string),
        "type": "HEX",
        "name": item label (string),
        "unit": unit (string) or null,
        "min": minimum value (integer),
        "max": maximum value (integer),
        "step": step size (integer),
        "value": current value (integer),
        "enableKeyId": "cfgN" (string, omitted if always enabled),
        "enableKeyValueMin": integer (omitted if always enabled),
        "enableKeyValueMax": integer (omitted if always enabled)
    }
    ```

- Boolean:

    ```json
    {
        "id": "cfgN" (string),
        "type": "BOOLEAN",
        "name": item label (string),
        "value": current value (boolean),
        "enableKeyId": "cfgN" (string, omitted if always enabled),
        "enableKeyValueMin": integer (omitted if always enabled),
        "enableKeyValueMax": integer (omitted if always enabled)
    }
    ```

- Enum:

    ```json
    {
        "id": "cfgN" (string),
        "type": "ENUM",
        "name": item label (string),
        "unit": unit (string) or null,
        "options": {
            "option1": value1 (integer),
            "option2": value2 (integer),
            "option3": value3 (integer),
            ...
        },
        "value": current value (integer),
        "enableKeyId": "cfgN" (string, omitted if always enabled),
        "enableKeyValueMin": integer (omitted if always enabled),
        "enableKeyValueMax": integer (omitted if always enabled)
    }
    ```

### setparams

Set LcdTap configuration parameters in bulk from the host.

- Command:

    ```json
    {
        "command": "setparams",
        "params": {
            "cfg0": value0 (integer/boolean),
            "cfg1": value1 (integer/boolean),
            ...
        }
    }
    ```

    - Keys are the `id` values returned by `getparams` (`"cfg0"` through `"cfg19"`, plus `"outputInterface"` and `"compositeDac"`).
    - It is not necessary to include all parameters; omitted parameters retain their current values.

- Response:

    ```json
    {"response": "ok"}
    ```

**Some changes reset the device.** The firmware saves the new values, sends the `ok` response, and then reboots. The USB CDC connection will drop and re-enumerate; reconnect before sending further commands. A reset happens when:

- `outputInterface` changes — each output binds its clocks and peripherals at startup (DVI-D and DisplayLink share clk_sys 312 MHz, NTSC 315 MHz, PAL 301.5 MHz).
- `cfg1` (bus interface) or `compositeDac` changes **while a composite mode is selected** — the DAC binds to its pins and peripheral at startup.

Changing `compositeDac` while `outputInterface` is `DVI-D` does not reset; nothing composite is running.

Reset is also the escape route if a composite or DisplayLink mode is selected with no display attached: USB CDC keeps working in every mode, so `setparams` with `"outputInterface": 0` restores DVI-D. Holding the LEFT key at power-on also boots with default settings.

### getframebuffer

Get the contents of the frame buffer.

- Command:

    ```json
    {
        "command": "getframebuffer",
        "writeProtected": whether to suppress writes while reading the frame buffer (boolean)
    }
    ```

- Response:

    ```json
    {
        "width": output source region width (integer),
        "height": output source region height (integer),
        "format": "RGB565-RLE",
        "data": RLE-compressed RGB565 image encoded as Base64 (string)
    }
    ```

    - Only the region specified by `outSrcX/Y/Width/Height` (the trim region) is sent, in pre-rotation physical buffer coordinates.
    - Brightness inversion is applied if active. Rotation is not applied.

- RLE format (`RGB565-RLE`):

    The pixel stream (row-major output order) is split into packets. Each
    packet starts with a control byte `c`:

    - `c & 0x80` set: run packet. The next 2 bytes are one RGB565 pixel
      (little-endian) repeated `(c & 0x7F) + 1` times (1..128 pixels).
    - `c & 0x80` clear: literal packet. The next `((c & 0x7F) + 1) * 2` bytes
      are `(c & 0x7F) + 1` raw RGB565 pixels (little-endian, 1..128 pixels).

    Runs never cross a row boundary. Adjacent literal packets are never
    emitted, which bounds the worst case (incompressible noise) at one control
    byte per 128 pixels, i.e. +0.39% over the raw size. The packet stream as a
    whole is Base64-encoded into `data`.

### dump_start

Start capturing a command dump.

- Command:

    ```json
    {"command": "cmddump_start"}
    ```

- Response:

    ```json
    {"response": "ok"}
    ```

### dump_abort

Abort a command dump capture.

- Command:

    ```json
    {"command": "cmddump_abort"}
    ```

- Response:

    ```json
    {"response": "ok"}
    ```

### dump_forcetrigger

Force-trigger a command dump capture.

- Command:

    ```json
    {"command": "cmddump_forcetrigger"}
    ```

- Response:

    ```json
    {"response": "ok"}
    ```

### dump_getstatus

Get the current status of the command dump.

- Command:

    ```json
    {"command": "cmddump_getstatus"}
    ```

- Response:

    ```json
    {"status": one of "WAIT", "ACTIVE", or "COMPLETE", "bytes": number of bytes stored in the buffer (integer)}
    ```

### dump_read

Get the contents of the command dump.

- Command:

    ```json
    {"command": "cmddump_read"}
    ```

- Response:

    ```json
    {
        "length": length of captured data (integer),
        "data": captured command data encoded as Base64 (string)
    }
    ```


### getstats

Get the debug statistics shown in the Statistics tab. The set of entries
depends on the example (pico2_universal reports TX/video counters,
pico2w_remote reports input-path counters only).

- Command:

    ```json
    {"command": "getstats"}
    ```

- Response:

    ```json
    {"stats": [
        {"name": display name (string),
         "value": counter value (unsigned integer),
         "unit": unit suffix, may be "" (string),
         "fmt": one of "dec", "hex", or "rate"},
        ...
    ]}
    ```

  `"hex"` values above `0xFF` mean "none yet". `"rate"` values are bytes
  per second.

### statsreset

Reset the statistics baselines (counters are free-running; reset is
implemented by baseline subtraction).

- Command:

    ```json
    {"command": "statsreset"}
    ```

- Response:

    ```json
    {"response": "ok"}
    ```

## pico2w_remote extensions

The pico2w_remote example speaks the same protocol over two transports:

- **USB CDC serial** — identical framing to the above.
- **HTTP** — `POST /api` with the JSON command as the request body
  (trailing CRLF optional). The response body is the same single JSON
  line, streamed with chunked transfer encoding. One request is served
  at a time; concurrent requests receive `503`.

String-valued parameters are accepted in `params` objects (used by
`setnetconfig` below). The following commands exist only on
pico2w_remote:

### getnetconfig

Read the stored network configuration. The passphrase is write-only:
only its presence is reported.

- Command:

    ```json
    {"command": "getnetconfig"}
    ```

- Response:

    ```json
    {"ssid": "...", "pskSet": true/false, "dhcp": true/false,
     "ip": "0.0.0.0", "netmask": "0.0.0.0", "gateway": "0.0.0.0",
     "dns": "0.0.0.0", "hostname": "lcdtap", "country": "JP"}
    ```

### setnetconfig

Update the network configuration. Omitted keys keep their current
values; an empty `psk` string selects an open network (omit the key to
keep the stored passphrase). The settings are saved to flash and applied
by an automatic reboot right after the response is flushed.

- Command:

    ```json
    {"command": "setnetconfig", "params": {
        "ssid": "MyAP", "psk": "secret", "dhcp": 1,
        "ip": "192.168.1.100", "netmask": "255.255.255.0",
        "gateway": "192.168.1.1", "dns": "192.168.1.1",
        "hostname": "lcdtap", "country": "JP"
    }}
    ```

- Response: `{"response": "ok"}` followed by a device reboot, or
  `{"error": "..."}`.

### netstatus

Read the live WiFi state.

- Command:

    ```json
    {"command": "netstatus"}
    ```

- Response:

    ```json
    {"state": one of "UNCONFIGURED", "CONNECTING", "CONNECTED", "FAILED",
     "ip": "0.0.0.0", "rssi": -50, "ssid": "...",
     "mac": "AA:BB:CC:DD:EE:FF", "hostname": "lcdtap"}
    ```
