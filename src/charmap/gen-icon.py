#!/usr/bin/python3
# Character Map's icon, drawn here: a white card with a grid of cells, one
# selected in the accent purple, and a large letter A drawn as strokes. Our
# own drawing, generated at build time.
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
    purple, grey, ink = (112, 48, 192, 255), (150, 150, 160, 255), (40, 40, 48, 255)
    d.rounded_rectangle([1 * u, 1 * u, 15 * u, 15 * u], radius=1.2 * u, fill=(255, 255, 255, 255),
                        outline=grey, width=max(1, int(0.5 * u)))
    for k in (1, 2):                  # grid lines: 3 x 3 cells
        p = (1 + 14 * k / 3) * u
        d.line([p, 1.5 * u, p, 14.5 * u], fill=grey, width=max(1, int(0.35 * u)))
        d.line([1.5 * u, p, 14.5 * u, p], fill=grey, width=max(1, int(0.35 * u)))
    c = 14 / 3 * u
    d.rectangle([1 * u + 2 * c + 0.3 * u, 1 * u + 2 * c + 0.3 * u, 15 * u - 0.6 * u, 15 * u - 0.6 * u], fill=purple)
    # a large A over the top-left four cells
    w = max(2, int(1.3 * u))
    d.line([2.5 * u, 10.5 * u, 5.5 * u, 2.5 * u], fill=ink, width=w)
    d.line([5.5 * u, 2.5 * u, 8.5 * u, 10.5 * u], fill=ink, width=w)
    d.line([3.7 * u, 7.6 * u, 7.3 * u, 7.6 * u], fill=ink, width=max(2, int(1.1 * u)))
    return im.resize((size, size), Image.LANCZOS)


sizes = [16, 24, 32, 48, 64, 256]
big = draw(256)
big.save(sys.argv[1], format="ICO", sizes=[(n, n) for n in sizes],
         append_images=[draw(n) for n in sizes[:-1]])
