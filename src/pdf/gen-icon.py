#!/usr/bin/python3
# SG PDF's icon, drawn here at build time (no artwork is committed): a
# white page with a folded corner, lines of text, a purple band, and a pen
# across its corner (it edits, too).
#
#   gen-icon.py OUT.ico
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import sgicon  # noqa: E402  (tools/sgicon.py: sizes, reduction, the .ico)

S = 1024  # drawn at 4x the largest size, then reduced (anti-aliasing)


def draw():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x0, y0, x1, y1, fold = 190, 70, S - 190, S - 70, 200
    shadow = [(x0 + 16, y0 + 24), (x1 - fold + 16, y0 + 24), (x1 + 16, y0 + fold + 24), (x1 + 16, y1 + 24), (x0 + 16, y1 + 24)]
    d.polygon(shadow, fill=(0, 0, 0, 60))
    page = [(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)]
    d.polygon(page, fill=(255, 255, 255, 255), outline=(0x9A, 0x9A, 0xA4, 255))
    d.line(page + [page[0]], fill=(0x9A, 0x9A, 0xA4, 255), width=14)
    # the folded corner
    d.polygon([(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)], fill=(0xDD, 0xD6, 0xEA, 255))
    d.line([(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)], fill=(0x9A, 0x9A, 0xA4, 255), width=14)
    # lines of text
    for i, y in enumerate(range(y0 + 150, y0 + 460, 78)):
        right = x1 - fold - 40 if y < y0 + fold + 20 else x1 - 90
        d.rounded_rectangle([x0 + 90, y, right - (i % 2) * 120, y + 34], radius=17, fill=(0xB8, 0xB8, 0xC4, 255))
    # the band: purple, with a white bar on it
    by0, by1 = y1 - 330, y1 - 110
    d.rectangle([x0 - 70, by0, x1 - 40, by1], fill=(112, 48, 192, 255))
    d.rounded_rectangle([x0 + 20, (by0 + by1) // 2 - 28, x1 - 170, (by0 + by1) // 2 + 28], radius=28, fill=(255, 255, 255, 255))
    # the pen: a charcoal barrel from the top right down over the page, a gold nib
    import math
    ang = math.radians(135)
    tipx, tipy = x1 - 250, y0 + 520
    dx, dy = -math.cos(ang), -math.sin(ang)          # up and to the right
    nx, ny = -dy, dx
    w, length, nib = 58, 430, 120
    def pt(a, b):
        return (tipx + dx * a + nx * b, tipy + dy * a + ny * b)
    d.polygon([pt(nib, -w), pt(nib + length, -w), pt(nib + length, w), pt(nib, w)], fill=(0x2E, 0x2E, 0x36, 255))
    d.polygon([pt(nib + length, -w), pt(nib + length + 50, -w + 12), pt(nib + length + 50, w - 12), pt(nib + length, w)],
              fill=(0x55, 0x55, 0x60, 255))
    d.polygon([pt(0, 0), pt(nib, -w), pt(nib, w)], fill=(0xF2, 0xB6, 0x1B, 255))
    d.polygon([pt(0, 0), pt(40, -18), pt(40, 18)], fill=(0x2E, 0x2E, 0x36, 255))
    return im


def main():
    sgicon.write_ico(sys.argv[1], draw())


if __name__ == "__main__":
    main()
