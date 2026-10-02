#!/usr/bin/env python3
"""gen-backdrop.py [--first] OUT.sgbd -- what the screen shows where no window is.

sg-compositor draws it beneath every window (its backdrop.c): the colour,
and this picture centred -- the four-diamond mark, "Getting things ready",
"This might take a minute." It shows while a session starts and nothing is
drawn yet: after the first-run setup, the first session takes a minute, and
it was a black screen. "Getting things ready" is for a person's first session
(--first, backdrop-first.sgbd, which sg-session-start chooses then); every
other sign-in shows "Welcome", as Windows does (David 2026-10-02: the first
sign-in's words at every sign-in).

The file: "SGBD", width, height, colour (0xAARRGGBB), little-endian uint32,
then the picture's premultiplied ARGB8888 pixels (little-endian: B, G, R, A).
"""
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

BG = (0x2A, 0x16, 0x4C)
MARK = [(0x7B, 0x3F, 0xD0), (0xE0, 0x40, 0x9A), (0xF0, 0xA0, 0x30), (0x1F, 0xB0, 0xA0)]  # top, right, bottom, left
W, H, K = 640, 300, 4   # drawn at 4x, scaled down: smooth edges
FONTS = ["/usr/share/fonts/opentype/inter/Inter-Light.otf", "/usr/share/fonts/opentype/inter/Inter-Regular.otf",
         "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]


def font(size, light=True):
    for path in (FONTS if light else FONTS[1:]):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def diamond(d, cx, cy, r, color):
    d.polygon([(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)], fill=color + (255,))


def main(out, first):
    img = Image.new("RGBA", (W * K, H * K), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # the mark: four diamonds around a centre, a small gap between them
    cx, cy, r, gap = W * K // 2, 70 * K, 26 * K, 3 * K
    off = r + gap
    diamond(d, cx, cy - off, r, MARK[0])
    diamond(d, cx + off, cy, r, MARK[1])
    diamond(d, cx, cy + off, r, MARK[2])
    diamond(d, cx - off, cy, r, MARK[3])
    lines = ((("Getting things ready", 34, 180, 255, True), ("This might take a minute.", 17, 232, 200, False))
             if first else (("Welcome", 34, 196, 255, True),))
    for text, size, y, alpha, light in lines:
        f = font(size * K, light)
        w = d.textlength(text, font=f)
        d.text(((W * K - w) / 2, y * K), text, font=f, fill=(255, 255, 255, alpha))
    img = img.resize((W, H), Image.LANCZOS)
    px = img.load()
    with open(out, "wb") as f:
        f.write(b"SGBD" + struct.pack("<III", W, H, 0xFF000000 | BG[0] << 16 | BG[1] << 8 | BG[2]))
        row = bytearray()
        for y in range(H):
            for x in range(W):
                r_, g_, b_, a = px[x, y]
                # premultiplied, as the compositor blends it
                row += bytes((b_ * a // 255, g_ * a // 255, r_ * a // 255, a))
        f.write(row)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--first"]
    main(args[0] if args else "backdrop.sgbd", "--first" in sys.argv[1:])
