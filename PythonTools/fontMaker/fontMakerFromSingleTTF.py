import re
from pathlib import Path
import freetype

# ─── Configuration ────────────────────────────────────────────────────────
TTF_FILE  = "UbuntuMono-Bold.ttf"   # TTF filename (must be in same folder as this script)
TTF_FILE  = "RobotoCondensed-Bold.ttf"   # TTF filename (must be in same folder as this script)
TTF_FILE  = "RobotoCondensed-Regular.ttf"   # TTF filename (must be in same folder as this script)
TTF_FILE  = "RobotoCondensed-Regular.ttf"   # TTF filename (must be in same folder as this script)
TTF_FILE  = "RobotoMono-Regular.ttf"   # TTF filename (must be in same folder as this script)
TTF_FILE  = "RobotoCondensed-Bold.ttf"   # TTF filename (must be in same folder as this script)
TTF_FILE  = "RobotoMono-Regular.ttf"   # TTF filename (must be in same folder as this script)

SIZE_PX   = 20                      # Target pixel height of REF_CHAR (auto-scales internally)
REF_CHAR  = '0'                       # Reference character used for pixel-height calibration
DPI       = 72                        # Resolution — keep at 72, adjust SIZE_PX instead
# Full printable ASCII range (space to ~), 95 characters:
FIRST   = 0x20                      # ' ' (space)
LAST    = 0x7E                      # '~'
# For clock/digits only (0-9 and :), 11 characters:
#FIRST     = 0x30                      # '0'
#LAST      = 0x3A                      # ':'
# ──────────────────────────────────────────────────────────────────────────

def pack_bitmap_to_1bpp(ft_bitmap):
    w = int(ft_bitmap.width)
    h = int(ft_bitmap.rows)
    pitch = int(ft_bitmap.pitch)
    buf = ft_bitmap.buffer
    pixel_mode = getattr(ft_bitmap, "pixel_mode", None)
    step = pitch
    if pitch < 0:
        pitch = -pitch
    def row_index(y):
        return (h - 1 - y) if step < 0 else y
    def mono_pixel_on(x, y):
        ri = row_index(y)
        byte = buf[ri * pitch + (x >> 3)]
        mask = 0x80 >> (x & 7)
        return (byte & mask) != 0
    def gray_pixel_on(x, y):
        ri = row_index(y)
        return buf[ri * pitch + x] > 0
    pixel_on = mono_pixel_on if pixel_mode == freetype.FT_PIXEL_MODE_MONO else gray_pixel_on
    out = bytearray()
    acc = 0
    bits = 0
    for y in range(h):
        for x in range(w):
            acc = (acc << 1) | (1 if pixel_on(x, y) else 0)
            bits += 1
            if bits == 8:
                out.append(acc & 0xFF)
                acc = 0
                bits = 0
    if bits:
        acc <<= (8 - bits)
        out.append(acc & 0xFF)
    return bytes(out), w, h

def main():
    script_dir = Path(__file__).resolve().parent
    ttf_file = str(script_dir / TTF_FILE)
    dpi = DPI
    first = FIRST
    last = LAST

    # Auto-scale: find SIZE_PT so that REF_CHAR bitmap height == SIZE_PX
    face = freetype.Face(ttf_file)
    face.set_char_size(SIZE_PX * 64, 0, dpi, dpi)   # initial guess: 1pt ≈ 1px at 72dpi
    face.load_char(REF_CHAR, freetype.FT_LOAD_DEFAULT)
    face.glyph.render(freetype.FT_RENDER_MODE_MONO)
    ref_h = face.glyph.bitmap.rows
    if ref_h > 0:
        size_pt = round(SIZE_PX * SIZE_PX / ref_h)
    else:
        size_pt = SIZE_PX
    print(f"[Font] '{REF_CHAR}' at initial {SIZE_PX}pt → {ref_h}px  →  adjusted to {size_pt}pt")
    face.set_char_size(size_pt * 64, 0, dpi, dpi)

    # Dynamically construct font_name and output_file
    ttf_stem = Path(ttf_file).stem.replace("-", "").replace(" ", "")
    font_name = f"{ttf_stem}{SIZE_PX}px7b"
    project_root = script_dir.parent.parent
    include_dir = project_root / "include"
    include_dir.mkdir(exist_ok=True)
    output_file = str(include_dir / f"{font_name}.h")
    y_advance = int(face.size.height) >> 6
    if y_advance <= 0:
        y_advance = (int(face.size.ascender) - int(face.size.descender)) >> 6
    if y_advance > 255:
        print(f"[Font] Warning: y_advance={y_advance} clamped to 255 (GFXfont uint8_t limit)")
        y_advance = 255

    bitmap_bytes = bytearray()
    glyph_entries = []
    for code in range(first, last + 1):
        if face.get_char_index(code) == 0 and code != 0:
            bitmap_offset = len(bitmap_bytes)
            # Clamp int8_t fields to -128..127
            glyph_entries.append((bitmap_offset, 0, 0, max(1, size_pt // 2), 0, 0))
            continue
        face.load_char(chr(code), freetype.FT_LOAD_DEFAULT)
        face.glyph.render(freetype.FT_RENDER_MODE_MONO)
        g = face.glyph
        b = g.bitmap
        packed, w, h = pack_bitmap_to_1bpp(b)
        bitmap_offset = len(bitmap_bytes)
        bitmap_bytes.extend(packed)
        x_advance = int(g.advance.x) >> 6
        x_offset = int(g.bitmap_left)
        y_offset = -int(g.bitmap_top)
        def clamp_int8(val):
            return max(-128, min(127, val))
        def clamp_uint8(val, field, code):
            if val > 255:
                print(f"[Font] Warning: char 0x{code:02X} ('{chr(code)}') {field}={val} clamped to 255 (uint8_t limit)")
                return 255
            return max(0, val)
        glyph_entries.append((bitmap_offset, clamp_uint8(w, 'width', code), clamp_uint8(h, 'height', code), clamp_uint8(x_advance, 'xAdvance', code), clamp_int8(x_offset), clamp_int8(y_offset)))

    guard = f"_{font_name.upper()}_H_"
    out = []
    out.append(f"#ifndef {guard}")
    out.append(f"#define {guard}")
    out.append("")
    out.append("#include <pgmspace.h>")
    out.append("#ifndef GFXfont")
    out.append("#  include <TFT_eSPI.h>  // provides GFXfont and GFXglyph")
    out.append("#endif")
    out.append("")
    out.append("// FreeFont (1bpp) generated from TTF by fontMakerFromSingleTTF.py")
    out.append(f"// Source: {ttf_file}")
    out.append(f"// Size: '{REF_CHAR}' = {SIZE_PX}px  (internal: {size_pt}pt @ {dpi}dpi), chars: 0x{first:02X}-0x{last:02X}")
    out.append("")
    def hex_bytes(data, indent="  ", cols=12):
        parts = [f"0x{b:02X}" for b in data]
        lines = []
        for i in range(0, len(parts), cols):
            lines.append(indent + ", ".join(parts[i : i + cols]))
        return ",\n".join(lines)
    out.append(f"const uint8_t {font_name}Bitmaps[] PROGMEM = {{")
    if bitmap_bytes:
        out.append(hex_bytes(bytes(bitmap_bytes)) + ",")
    out.append("};")
    out.append("")
    out.append(f"const GFXglyph {font_name}Glyphs[] PROGMEM = {{")
    for bo, w, h, xa, xo, yo in glyph_entries:
        out.append(f"  {{ {bo}, {w}, {h}, {xa}, {xo}, {yo} }},")
    out.append("};")
    out.append("")
    out.append(f"const GFXfont {font_name} PROGMEM = {{")
    out.append(f"  (uint8_t  *){font_name}Bitmaps,")
    out.append(f"  (GFXglyph *){font_name}Glyphs,")
    out.append(f"  0x{first:02X}, 0x{last:02X}, {y_advance}")
    out.append("};")
    out.append("")
    out.append(f"#endif // {guard}")
    out.append("")
    Path(output_file).write_text("\n".join(out), encoding="utf-8")
    print(f"Font generated and saved to {output_file}")

if __name__ == "__main__":
    main()