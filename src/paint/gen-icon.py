#!/usr/bin/python3
# sg-paint's icon, drawn here: an artist's palette with paint wells and a
# brush, in the Stained Glass colours. Our own drawing, generated at build
# time (no binary is committed).
#
#   gen-icon.py OUT.ico
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import sys

from PIL import Image, ImageDraw

N = 1024  # drawn large, reduced with a good filter
img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
s = N / 64

def S(*v):
    return [x * s for x in v]

# the palette: a rounded board with a thumb hole
d.ellipse(S(4, 10, 60, 58), fill=(250, 244, 236, 255), outline=(150, 110, 70, 255), width=int(2.5 * s))
d.ellipse(S(38, 38, 48, 48), fill=(0, 0, 0, 0), outline=(150, 110, 70, 255), width=int(2.5 * s))
for (x, y, c) in [(12, 22, (112, 48, 192)), (22, 15, (237, 28, 36)), (34, 14, (255, 201, 14)),
                  (46, 19, (34, 177, 76)), (13, 35, (0, 162, 232)), (22, 45, (63, 72, 204))]:
    d.ellipse(S(x, y, x + 9, y + 9), fill=c + (255,))
# the brush, across the palette
d.line(S(62, 2, 34, 32), fill=(214, 160, 90, 255), width=int(5 * s))
d.polygon(S(36, 28, 40, 32, 30, 44, 20, 48, 26, 38), fill=(60, 40, 90, 255))
d.polygon(S(20, 48, 26, 38, 30, 44), fill=(112, 48, 192, 255))

sizes = [16, 24, 32, 48, 64, 256]
img.resize((256, 256), Image.LANCZOS).save(sys.argv[1], sizes=[(z, z) for z in sizes])
