#!/usr/bin/python3
# On-Screen Keyboard's icon, drawn here: a dark keyboard with rows of light
# keys, the space bar in the accent purple. Our own drawing, generated at
# build time.
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


def draw(size):
    s = size                          # sgicon draws each frame at 4x and reduces it
    u = s / 16.0
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    body, key, purple = (48, 48, 56, 255), (225, 225, 232, 255), (112, 48, 192, 255)
    d.rounded_rectangle([0.5 * u, 3.5 * u, 15.5 * u, 13 * u], radius=1.2 * u, fill=body)
    for row, (y, n, x0) in enumerate(((4.6, 6, 1.6), (6.8, 6, 2.1), (9.0, 5, 2.6))):
        w = (15.5 - 1.1 - x0) / n
        for i in range(n):
            x = x0 + i * w
            d.rectangle([x * u, y * u, (x + w - 0.5) * u, (y + 1.6) * u], fill=key)
    d.rectangle([4.2 * u, 11.1 * u, 11.8 * u, 12.2 * u], fill=purple)
    return im


sgicon.write_ico(sys.argv[1], draw)
