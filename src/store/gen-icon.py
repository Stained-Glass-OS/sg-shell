#!/usr/bin/python3
# SG Store's icon, drawn here at build time (no artwork is committed): a
# shopping bag on a purple disc with a download arrow -- an app store.
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

S = 1024


def draw():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c, r = S // 2, 420
    d.ellipse([c - r, c - r, c + r, c + r], fill=(112, 48, 192, 255))
    white, w = (255, 255, 255, 255), 40
    # the bag body
    bx0, by0, bx1, by1 = c - 250, c - 120, c + 250, c + 300
    d.rounded_rectangle([bx0, by0, bx1, by1], radius=60, outline=white, width=w)
    # the handle
    hy = by0
    d.arc([c - 150, hy - 190, c + 150, hy + 60], start=180, end=360, fill=white, width=w)
    # a download arrow inside the bag
    ax, ay = c, c + 70
    d.line([(ax, ay - 120), (ax, ay + 60)], fill=white, width=40)
    d.polygon([(ax - 70, ay + 10), (ax + 70, ay + 10), (ax, ay + 95)], fill=white)
    return im


def main():
    sgicon.write_ico(sys.argv[1], draw())


if __name__ == "__main__":
    main()
