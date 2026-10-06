#!/usr/bin/python3
# Snipping Tool's icon, drawn here: a purple tile, a white selection frame with
# corner handles, and the snipped piece inside it. Written as a multi-size .ico.
#
#   sg-snip-icon.py OUT.ico
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import sgicon  # noqa: E402  (tools/sgicon.py: sizes, reduction, the .ico)

N = 1024                       # drawn big, then scaled down for each size
im = Image.new("RGBA", (N, N), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
u = N / 64
d.rounded_rectangle([4 * u, 4 * u, 60 * u, 60 * u], radius=12 * u, fill=(112, 48, 192, 255))
d.rounded_rectangle([4 * u, 34 * u, 60 * u, 60 * u], radius=12 * u, fill=(96, 38, 170, 255))
d.rectangle([4 * u, 34 * u, 60 * u, 46 * u], fill=(96, 38, 170, 255))
# the piece that was snipped
d.rectangle([20 * u, 22 * u, 44 * u, 42 * u], fill=(214, 190, 245, 255))
# the frame round it: dashes along each side and a handle at each corner
w = int(2.5 * u)
for x0 in range(17, 47, 6):
    d.line([(x0 * u, 17 * u), ((x0 + 3) * u, 17 * u)], fill="white", width=w)
    d.line([(x0 * u, 47 * u), ((x0 + 3) * u, 47 * u)], fill="white", width=w)
for y0 in range(17, 47, 6):
    d.line([(14 * u, y0 * u), (14 * u, (y0 + 3) * u)], fill="white", width=w)
    d.line([(50 * u, y0 * u), (50 * u, (y0 + 3) * u)], fill="white", width=w)
for cx, cy in ((14, 17), (50, 17), (14, 47), (50, 47)):
    d.rectangle([(cx - 3.5) * u, (cy - 3.5) * u, (cx + 3.5) * u, (cy + 3.5) * u], fill="white")
sgicon.write_ico(sys.argv[1], im)
