"""sgicon: how every icon we ship is made, in one place.

Our icons are drawn by small Python programs (src/**/gen-icon*.py,
src/sg-*-icon.py, office/gen-icons.py): shapes at coordinates on a large
canvas -- vector drawing, no artwork committed and none of anyone else's.
This module turns such a drawing into the files we ship:

  * .ico with every frame Windows programs ask for at 100-250% display scale:
    SIZES below. Each frame is reduced straight from the large drawing (not
    from a smaller frame), so edges are exact at every size. Frames up to 24 px
    are sharpened a little after reduction, so 1-2 px strokes stay crisp
    rather than grey ("hinting" by the frame, not the outline). A drawing may
    give its own picture for small sizes (fewer details), see frames().
    Frames below 256 px are stored as 32-bit DIBs (what every reader takes),
    the 256 px frame as PNG (as Windows' own icons are).
  * hicolor PNGs for Linux menus and the Store (write_hicolor).

  write_ico(path, draw)            draw(S) -> an RGBA image S x S, any S: each
                                   frame drawn at 4x its size and reduced
  write_ico(path, image)           a large RGBA image already drawn (1024 px)
  write_ico(path, src, small=f)    f(size) -> RGBA image for sizes <= 32 (or None)

Gate: test/icon-sizes-check.sh checks every .ico the build makes has SIZES.
Mutant: SG_MUTANT_ICON_SIZES=1 in the environment writes the old six sizes.

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import io
import os
import struct

from PIL import Image, ImageFilter

SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
HICOLOR = (16, 22, 24, 32, 48, 64, 96, 128, 256, 512)
CANVAS = 1024
OVERSAMPLE = 4   # a drawn frame: drawn this many times larger, then reduced

if os.environ.get("SG_MUTANT_ICON_SIZES"):
    SIZES = (16, 24, 32, 48, 64, 256)


def _master(src, size=CANVAS):
    if callable(src):
        src = src(size)
    if src.mode != "RGBA":
        src = src.convert("RGBA")
    return src


def reduce(big, n):
    """big reduced to n x n: premultiplied (no dark fringes at the edges),
    Lanczos from the large drawing, a light sharpening at the smallest sizes."""
    if big.size == (n, n):
        return big.copy()
    # premultiply so transparent pixels' colour does not bleed into edges
    pm = big.convert("RGBa").resize((n, n), Image.LANCZOS)
    im = pm.convert("RGBA")
    if n <= 24:
        r, g, b, a = im.split()
        rgb = Image.merge("RGB", (r, g, b)).filter(ImageFilter.UnsharpMask(radius=0.6, percent=45, threshold=0))
        a = a.filter(ImageFilter.UnsharpMask(radius=0.6, percent=35, threshold=0))
        im = Image.merge("RGBA", (*rgb.split(), a))
    return im


def frames(src, sizes=SIZES, small=None):
    """{size: RGBA image} for each size. src is a large image (each frame
    reduced from it), or draw(S) -> an S x S image: then each frame is drawn
    afresh at four times its size and reduced (a vector drawing rendered for
    the frame, so a drawing can keep strokes whole at small sizes). small(n),
    for n <= 32, may give that frame's own drawing (or None)."""
    out = {}
    for n in sizes:
        pic = small(n) if small and n <= 32 else None
        if pic is None:
            pic = src(OVERSAMPLE * n) if callable(src) else src
        pic = _master(pic, n)
        out[n] = reduce(pic, n)
    return out


def _dib(im):
    """an icon frame as a 32-bit BITMAPINFOHEADER DIB with its AND mask"""
    n = im.size[0]
    px = im.tobytes("raw", "BGRA")
    rows = [px[y * n * 4:(y + 1) * n * 4] for y in range(n)]
    xor = b"".join(reversed(rows))        # bottom-up
    stride = ((n + 31) // 32) * 4
    alpha = im.getchannel("A").tobytes()
    mask = bytearray()
    for y in reversed(range(n)):
        row = bytearray(stride)
        for x in range(n):
            if alpha[y * n + x] == 0:
                row[x >> 3] |= 0x80 >> (x & 7)
        mask += row
    hdr = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
    return hdr + xor + bytes(mask)


def ico_bytes(frame_map):
    sizes = sorted(frame_map)
    blobs = []
    for n in sizes:
        im = frame_map[n]
        if n >= 256:
            b = io.BytesIO()
            im.save(b, format="PNG", optimize=True)
            blobs.append(b.getvalue())
        else:
            blobs.append(_dib(im))
    out = struct.pack("<HHH", 0, 1, len(sizes))
    off = 6 + 16 * len(sizes)
    for n, blob in zip(sizes, blobs):
        out += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(blob), off)
        off += len(blob)
    return out + b"".join(blobs)


def write_ico(path, src, sizes=SIZES, small=None):
    """write path: an .ico of src at every size"""
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "wb") as f:
        f.write(ico_bytes(frames(src, sizes, small)))


def write_hicolor(root, name, src, sizes=HICOLOR, small=None):
    """root/<n>x<n>/apps/name.png for each size (an icon theme's tree)"""
    for n, im in frames(src, sizes, small).items():
        d = os.path.join(root, "%dx%d" % (n, n), "apps")
        os.makedirs(d, exist_ok=True)
        im.save(os.path.join(d, name + ".png"), optimize=True)


def read_ico_sizes(path):
    """[(width, height, 'png'|'dib')] of an .ico's frames (for the gate)"""
    with open(path, "rb") as f:
        data = f.read()
    _, kind, count = struct.unpack_from("<HHH", data, 0)
    out = []
    for i in range(count):
        w, h, _, _, _, _, size, off = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        fmt = "png" if data[off:off + 8] == b"\x89PNG\r\n\x1a\n" else "dib"
        out.append((w or 256, h or 256, fmt))
    return out
