"""
gen_font_st7032.py - Bitmap font image to C++ header converter

Reads lib/image/font_st7032.png, which contains glyph designs for character
codes 0x00 through 0xFF on a black background with white pixels. The image
is arranged as 16 glyphs per row, each glyph being 5x8 pixels, with no
gaps between them. Each row of a glyph is encoded as one byte with MSB-first
bit order (leftmost pixel maps to bit 4). The resulting 8-byte-per-glyph
data is written to lib/include/lcdtap/font_st7032.hpp as a static C++ array
inside the lcdtap::font_st7032 namespace.
"""

import sys
from pathlib import Path
from PIL import Image

SCRIPT_DIR = Path(__file__).parent
INPUT_PATH = SCRIPT_DIR / "image" / "font_st7032.png"
OUTPUT_PATH = SCRIPT_DIR / "include" / "lcdtap" / "font_st7032.hpp"

CHAR_W = 5
CHAR_H = 8
FIRST_CODE = 0x00
LAST_CODE = 0xFF
ROWS = 16

NUM_CHARS = LAST_CODE - FIRST_CODE + 1
COLS = (NUM_CHARS + ROWS - 1) // ROWS

EXPECTED_W = ROWS * CHAR_W
EXPECTED_H = COLS * CHAR_H


def encode_glyph(img, col, row):
    """Extract and encode one glyph as 8 bytes (MSB-first per row)."""
    x0 = col * (CHAR_W + 1)
    y0 = row * (CHAR_H + 1)
    data = []
    for y in range(CHAR_H):
        byte = 0
        for x in range(CHAR_W):
            pixel = img.getpixel((x0 + x, y0 + y))
            # Luminance: treat any channel value > 127 as white
            value = pixel[0] if isinstance(pixel, tuple) else pixel
            if value > 127:
                byte |= 1 << (CHAR_W - 1 - x)  # MSB-first: leftmost pixel is bit 4
        data.append(byte)
    return data


def main():
    img = Image.open(INPUT_PATH).convert("RGB")

    if img.width < EXPECTED_W or img.height < EXPECTED_H:
        print(
            f"Error: image too small; need at least {EXPECTED_W}x{EXPECTED_H}, "
            f"got {img.width}x{img.height}",
            file=sys.stderr,
        )
        sys.exit(1)

    all_bytes = []
    for char_index in range(NUM_CHARS):
        col = char_index // COLS
        row = char_index % COLS
        all_bytes.extend(encode_glyph(img, col, row))

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)

    BYTES_PER_LINE = 16
    lines = []
    for i in range(0, len(all_bytes), BYTES_PER_LINE):
        chunk = all_bytes[i : i + BYTES_PER_LINE]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")

    array_body = "\n".join(lines)

    guard = "LCDTAP_FONT_ST7032_HPP"
    content = f"""\
#ifndef {guard}
#define {guard}

#include <cstdint>

namespace lcdtap {{
namespace font_st7032 {{

static constexpr int GLYPH_WIDTH = {CHAR_W};
static constexpr int GLYPH_HEIGHT = {CHAR_H};
static constexpr char CODE_FIRST = 0x{FIRST_CODE:02x};
static constexpr char CODE_LAST = 0x{LAST_CODE:02x};

// clang-format off
static const uint8_t bitmap[] = {{
{array_body}
}};
// clang-format on

}} // namespace font_st7032
}} // namespace lcdtap

#endif // {guard}
"""

    OUTPUT_PATH.write_text(content, encoding="utf-8")
    print(f"Written: {OUTPUT_PATH}")
    print(f"  {NUM_CHARS} glyphs, {len(all_bytes)} bytes total")


if __name__ == "__main__":
    main()
