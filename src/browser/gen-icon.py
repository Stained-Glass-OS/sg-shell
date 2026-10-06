#!/usr/bin/python3
# Get a web browser's icon, drawn here at build time (no artwork is
# committed): a globe of meridians and parallels on a purple disc, with a
# download arrow.
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
    white, w = (255, 255, 255, 255), 34
    g = 300  # the globe
    d.ellipse([c - g, c - g, c + g, c + g], outline=white, width=w)
    d.ellipse([c - g // 2, c - g, c + g // 2, c + g], outline=white, width=w)
    d.line([(c, c - g), (c, c + g)], fill=white, width=w)
    d.line([(c - g, c), (c + g, c)], fill=white, width=w)
    for y in (c - g // 2 - 20, c + g // 2 + 20):
        half = int((g * g - (y - c) ** 2) ** 0.5)
        d.line([(c - half, y), (c + half, y)], fill=white, width=w)
    # the download arrow, bottom right, on its own disc
    a = 250
    ax, ay = c + 250, c + 250
    d.ellipse([ax - a // 2 - 20, ay - a // 2 - 20, ax + a // 2 + 20, ay + a // 2 + 20], fill=(255, 255, 255, 255))
    d.ellipse([ax - a // 2, ay - a // 2, ax + a // 2, ay + a // 2], fill=(0x3E, 0x16, 0x7A, 255))
    d.line([(ax, ay - 70), (ax, ay + 50)], fill=white, width=36)
    d.polygon([(ax - 60, ay + 10), (ax + 60, ay + 10), (ax, ay + 80)], fill=white)
    return im


def main():
    sgicon.write_ico(sys.argv[1], draw())


if __name__ == "__main__":
    main()
