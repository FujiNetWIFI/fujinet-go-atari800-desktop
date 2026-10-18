#!/usr/bin/env python3
"""Generate roms_embedded.c for a WITH_ATARI800_ROMS developer build.

    embed-roms.py <out.c> <dir>     embed the recognised images in <dir>
    embed-roms.py --none <out.c>    an empty table (every published build)

An image is recognised by size and CRC-32 (the system ROMs atari800 knows)
or by its name: <slot key>.rom, the names core/src/roms.c stores imports
under (e.g. xep80_charset.rom). Never ship a build made with images.
"""
import sys
import zlib
from pathlib import Path

KNOWN = {  # (size, crc32): slot key -- core/src/roms.c s_slots[]
    (0x2800, 0xc1b3bb02): "os_a_ntsc", (0x2800, 0x72b3fed4): "os_a_pal",
    (0x2800, 0x0e86d61d): "os_b_ntsc", (0x4000, 0xc5c11546): "os_aa00r10",
    (0x4000, 0x1a1d7b1b): "os_aa01r11", (0x4000, 0x643bcc98): "os_bb00r1",
    (0x4000, 0x1f9cd270): "os_bb01r2", (0x4000, 0x0d477aa1): "os_bb02r3",
    (0x4000, 0xd425a9cf): "os_bb02r3v4", (0x4000, 0x0e000b99): "os_cc01r4",
    (0x4000, 0x29f133f7): "os_bb01r3", (0x4000, 0x1eaf4002): "os_bb01r4",
    (0x4000, 0x45f47988): "os_bb01r59", (0x4000, 0xf0a236d3): "os_bb01r59a",
    (0x2000, 0x4bec4de2): "basic_a", (0x2000, 0xf0202fb3): "basic_b",
    (0x2000, 0x7d684184): "basic_c", (0x2000, 0xbdca01fb): "xegame",
}
NAMED = {"xld_voice", "xld_disk", "mio", "black_box", "proto80", "af80",
         "af80_charset", "bit3", "bit3_charset", "xep80_charset",
         "os800_custom", "osxl_custom", "basic_custom", "xegame_custom"}


def main():
    args = sys.argv[1:]
    none = args and args[0] == "--none"
    if none:
        args = args[1:]
    out = Path(args[0])
    found = {}
    if not none:
        for f in sorted(Path(args[1]).glob("*")):
            if not f.is_file() or f.suffix == ".py":
                continue
            data = f.read_bytes()
            key = KNOWN.get((len(data), zlib.crc32(data) & 0xffffffff))
            if not key and f.stem in NAMED:
                key = f.stem
            if key:
                found[key] = data
    lines = ['#include "roms_embedded.h"', ""]
    for i, (key, data) in enumerate(sorted(found.items())):
        lines.append(f"static const uint8_t rom{i}[] = {{")
        for j in range(0, len(data), 16):
            lines.append("    " + ",".join(f"0x{b:02x}" for b in data[j:j + 16]) + ",")
        lines.append("};")
    lines.append("const a800_embedded_rom a800_embedded_roms[] = {")
    for i, (key, data) in enumerate(sorted(found.items())):
        lines.append(f'    {{"{key}", rom{i}, {len(data)}u}},')
    lines.append('    {0, 0, 0}')
    lines.append("};")
    lines.append(f"const unsigned a800_embedded_rom_count = {len(found)};")
    out.write_text("\n".join(lines) + "\n")
    print(f"embed-roms: {len(found)} image(s) -> {out}")


if __name__ == "__main__":
    main()
