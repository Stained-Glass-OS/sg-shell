#!/usr/bin/python3
# Linux Terminal (Administrator)'s icon, drawn here: a dark terminal window
# with a root prompt, "#", in the accent purple and a cursor. Our own drawing,
# generated at build time.
#
#   gen-icon.py OUT.ico
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import sys

from PIL import Image, ImageDraw


def draw(size):
    s = 4 * size                      # drawn at 4x, filtered down
    u = s / 16.0
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0.5 * u, 1.5 * u, 15.5 * u, 14.5 * u], radius=1.3 * u, fill=(24, 24, 28, 255),
                        outline=(96, 96, 104, 255), width=max(1, int(0.5 * u)))
    d.rectangle([0.9 * u, 1.9 * u, 15.1 * u, 3.6 * u], fill=(64, 64, 72, 255))    # title bar
    w = max(2, int(1.1 * u))
    purple = (170, 110, 255, 255)
    # "#": two uprights and two bars
    d.line([4.2 * u, 5.6 * u, 3.6 * u, 11.4 * u], fill=purple, width=w)
    d.line([6.6 * u, 5.6 * u, 6.0 * u, 11.4 * u], fill=purple, width=w)
    d.line([2.6 * u, 7.3 * u, 7.8 * u, 7.3 * u], fill=purple, width=w)
    d.line([2.3 * u, 9.7 * u, 7.5 * u, 9.7 * u], fill=purple, width=w)
    d.rectangle([9.2 * u, 10.2 * u, 12.6 * u, 11.4 * u], fill=(220, 220, 225, 255))  # cursor
    return im.resize((size, size), Image.LANCZOS)


sizes = [16, 24, 32, 48, 64, 256]
big = draw(256)
big.save(sys.argv[1], format="ICO", sizes=[(n, n) for n in sizes],
         append_images=[draw(n) for n in sizes[:-1]])
