#!/usr/bin/env python3
"""Make BigTermCLI's console fonts (PSF2).

    pip install --user pillow
    python tools/make_fonts.py TERMINUS_BDF_DIR KERNEL_LIB_FONTS_DIR

Two faces, kept in the repo (pkg/usr/share/displaymod/fonts/) with their
licenses, so the normal build needs none of the sources:

- Terminus 12/14/16/18/20 (ter-u*n.bdf): the CardputerZero OS terminal font.
  A bitmap face, converted glyph for glyph.
- VGA 8x8 and 8x16: the Linux kernel's console fonts (lib/fonts), as they are.

256 glyphs (512 would cost the console its bright colors). The console clears
cells with glyph 32 itself, not through the Unicode table, so ASCII stays at
its own codes and slot 32 is the space.
"""

import gzip
import io
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = [os.path.join(ROOT, "pkg", "usr", "share", "displaymod", "fonts"),
       os.path.join(ROOT, "legacy", "pkg", "usr", "share", "displaymod", "fonts")]

EXTRA = ("─│┌┐└┘├┤┬┴┼═║╔╗╚╝╠╣╦╩╬╭╮╯╰"   # box drawing (tmux borders, menus)
         "█▀▄▌▐░▒▓"                         # blocks
         "←↑→↓↵•…–—‘’“”€‹›"                 # arrows, punctuation
         "▲▼◀▶■□●○◆✓✗≤≥≠�")

# CP437's pictures in slots 1-31 and 127 (the VGA fonts are in CP437 order)
CP437_LOW = "☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼"


# ------------------------------------------------------------------ layout

def latin_slots(have):
    """ASCII and Latin-1 at their own codes, extras in 1-31 and 127-159."""
    slots = [None] * 256
    for c in list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)):
        if have(chr(c)):
            slots[c] = chr(c)
    free = list(range(1, 0x20)) + list(range(0x7F, 0xA0))
    extras = [c for c in EXTRA if have(c)]
    for slot, c in zip(free, extras):
        slots[slot] = c
    return slots


def cp437_slots():
    slots = [None] + list(CP437_LOW) + [chr(c) for c in range(0x20, 0x7F)] + ["⌂"]
    slots += [bytes([c]).decode("cp437") for c in range(0x80, 0x100)]
    slots[0x20] = " "
    return slots


# ------------------------------------------------------------------ sources

def terminus(path):
    """Cells {char: rows of 0/1}, width, height from a BDF file."""
    glyphs, w, h, ascent = {}, 0, 0, 0
    with open(path, encoding="latin-1") as fh:
        lines = iter(fh.read().splitlines())
    for line in lines:
        k = line.split()
        if not k:
            continue
        if k[0] == "FONTBOUNDINGBOX":
            w, h = int(k[1]), int(k[2])
        elif k[0] == "FONT_ASCENT":
            ascent = int(k[1])
        elif k[0] == "STARTCHAR":
            enc, bbx = None, None
            for line in lines:
                k = line.split()
                if k[0] == "ENCODING":
                    enc = int(k[1])
                elif k[0] == "BBX":
                    bbx = [int(v) for v in k[1:5]]
                elif k[0] == "BITMAP":
                    bw, bh, bx, by = bbx
                    cell = [[0] * w for _ in range(h)]
                    top = ascent - (by + bh)
                    nbits = ((bw + 7) // 8) * 8
                    for r in range(bh):
                        bits = int(next(lines).strip(), 16)
                        for x in range(bw):
                            if bits & (1 << (nbits - 1 - x)):
                                y, xx = top + r, bx + x
                                if 0 <= y < h and 0 <= xx < w:
                                    cell[y][xx] = 1
                    if enc is not None and enc >= 0:
                        glyphs[chr(enc)] = cell
                elif k[0] == "ENDCHAR":
                    break
    return glyphs, w, h



def kernel_vga(path, h):
    with open(path, encoding="latin-1") as fh:
        s = fh.read()
    data = [int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})\s*,", s[s.index("fontdata_"):])]
    if len(data) != 256 * h:
        sys.exit("%s: %d bytes, expected %d" % (path, len(data), 256 * h))
    cells = []
    for i in range(256):
        cells.append([[(data[i * h + y] >> (7 - x)) & 1 for x in range(8)] for y in range(h)])
    return cells


# ------------------------------------------------------------------ output

def psf2(w, h, cells, slots):
    bpr = (w + 7) // 8
    data = b""
    for cell in cells:
        for row in cell:
            bits = 0
            for x, on in enumerate(row):
                if on:
                    bits |= 1 << (bpr * 8 - 1 - x)
            data += bits.to_bytes(bpr, "big")
    hdr = struct.pack("<IIIIIIII", 0x864AB572, 0, 32, 1, 256, bpr * h, h, w)
    table = b"".join((c.encode("utf-8") if c else b"") + b"\xff" for c in slots)
    return hdr + data + table


def save(name, w, h, cells, slots):
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as gz:
        gz.write(psf2(w, h, cells, slots))
    for d in OUT:
        with open(os.path.join(d, name), "wb") as fh:
            fh.write(buf.getvalue())
    print("%-24s %2dx%-2d cell, %dx%d on 320x240" % (name, w, h, 320 // w, 240 // h))


def from_glyphs(glyphs, w, h):
    slots = latin_slots(lambda c: c in glyphs)
    blank = [[0] * w for _ in range(h)]
    return [glyphs[c] if c else blank for c in slots], slots


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ter, kdir = sys.argv[1:]
    for d in OUT:
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):                       # replace all fonts
            if f.endswith(".psf.gz"):
                os.remove(os.path.join(d, f))
    for px in (12, 14, 16, 18, 20):
        g, w, h = terminus(os.path.join(ter, "ter-u%dn.bdf" % px))
        save("ter-u%dn.psf.gz" % px, w, h, *from_glyphs(g, w, h))
    for h in (8, 16):
        save("vga8x%d.psf.gz" % h, 8, h, kernel_vga(os.path.join(kdir, "font_8x%d.c" % h), h), cp437_slots())


if __name__ == "__main__":
    main()
