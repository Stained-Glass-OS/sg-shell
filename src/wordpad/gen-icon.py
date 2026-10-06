#!/usr/bin/python3
# sg-wordpad's icon, drawn here: a page with lines of text of different
# lengths and a pen nib across its corner, in the Stained Glass colours. Our
# own drawing, generated at build time (no binary is committed).
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

N = 1024
img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
s = N / 64


def S(*v):
    return [x * s for x in v]


# the page, with a folded corner
d.polygon(S(10, 4, 44, 4, 54, 14, 54, 60, 10, 60), fill=(255, 255, 255, 255), outline=(112, 48, 192, 255))
d.line(S(10, 4, 44, 4, 54, 14, 54, 60, 10, 60, 10, 4), fill=(112, 48, 192, 255), width=int(2.5 * s))
d.polygon(S(44, 4, 44, 14, 54, 14), fill=(214, 196, 240, 255), outline=(112, 48, 192, 255))
# a heading and lines of text
d.rectangle(S(16, 12, 36, 16), fill=(112, 48, 192, 255))
for i, w in enumerate([32, 28, 32, 22, 30, 18]):
    y = 22 + i * 6
    d.rectangle(S(16, y, 16 + w, y + 2.4), fill=(90, 90, 110, 255))
# the pen nib, bottom right
d.polygon(S(62, 34, 38, 58, 34, 62, 36, 56, 58, 32), fill=(63, 72, 204, 255))
d.polygon(S(36, 56, 34, 62, 40, 60), fill=(40, 30, 60, 255))

sgicon.write_ico(sys.argv[1], img)
