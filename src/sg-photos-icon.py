#!/usr/bin/python3
# The Photos app's icon, drawn here at build time (no artwork is committed):
# a purple tile holding a picture of hills under a sun.
#
#   sg-photos-icon.py OUT.ico
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import sys

from PIL import Image, ImageDraw

S = 1024  # drawn at 4x the largest size, then reduced (anti-aliasing)


def draw():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # the tile: a vertical purple gradient in a rounded square
    tile = Image.new("RGBA", (S, S))
    td = ImageDraw.Draw(tile)
    for y in range(S):
        t = y / (S - 1)
        c = (int(0x8A + (0x5A - 0x8A) * t), int(0x44 + (0x22 - 0x44) * t), int(0xD8 + (0xA8 - 0xD8) * t), 255)
        td.line([(0, y), (S, y)], fill=c)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([40, 40, S - 40, S - 40], radius=190, fill=255)
    im.paste(tile, (0, 0), mask)
    # the picture: a white frame with a sky, a sun and two hills
    x0, y0, x1, y1 = 200, 250, S - 200, S - 250
    d.rounded_rectangle([x0, y0, x1, y1], radius=40, fill=(255, 255, 255, 255))
    ix0, iy0, ix1, iy1 = x0 + 46, y0 + 46, x1 - 46, y1 - 46
    pic = Image.new("RGBA", (ix1 - ix0, iy1 - iy0), (0xD9, 0xEC, 0xFF, 255))
    pd = ImageDraw.Draw(pic)
    w, h = pic.size
    pd.ellipse([w * 0.62, h * 0.12, w * 0.82, h * 0.12 + w * 0.20], fill=(0xFF, 0xB8, 0x1C, 255))
    pd.polygon([(0, h), (0, h * 0.62), (w * 0.30, h * 0.30), (w * 0.62, h), ], fill=(0x6A, 0x2E, 0xB8, 255))
    pd.polygon([(w * 0.28, h), (w * 0.64, h * 0.45), (w, h * 0.78), (w, h)], fill=(0x3E, 0x16, 0x7A, 255))
    im.paste(pic, (ix0, iy0))
    return im


def main():
    big = draw()
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    big.resize((256, 256), Image.LANCZOS).save(sys.argv[1], format="ICO", sizes=[(s, s) for s in sizes])


if __name__ == "__main__":
    main()
