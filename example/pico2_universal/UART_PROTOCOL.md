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

The `id` field of each parameter is `"cfg0"`, `"cfg1"`, ..., `"cfg14"`, corresponding to the `ConfigId` enum index in the firmware. Use the same ids as keys in `setparams`.

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

    - Keys are the `id` values returned by `getparams` (`"cfg0"` through `"cfg14"`).
    - It is not necessary to include all parameters; omitted parameters retain their current values.

- Response:

    ```json
    {"response": "ok"}
    ```

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
        "format": "RGB565",
        "data": RGB565 image encoded as Base64 in little-endian byte order (string)
    }
    ```

    - Only the region specified by `outSrcX/Y/Width/Height` (the trim region) is sent, in pre-rotation physical buffer coordinates.
    - Brightness inversion is applied if active. Rotation is not applied.

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
