#!/usr/bin/python3
# Media Player's icon, drawn here (no borrowed artwork): a rounded tile in the
# Stained Glass purple, a lighter pane like a sheet of glass, and a white play
# triangle. Written as a multi-size .ico for windres.
#
#   sg-media-icon.py OUT.ico
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import sgicon  # noqa: E402  (tools/sgicon.py: sizes, reduction, the .ico)

N = 1024
img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
top, bottom = (0x8A, 0x3C, 0xD8), (0x4B, 0x1C, 0x7A)
grad = Image.new("RGBA", (N, N))
gd = ImageDraw.Draw(grad)
for y in range(N):
    t = y / (N - 1)
    gd.line([(0, y), (N, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
mask = Image.new("L", (N, N), 0)
ImageDraw.Draw(mask).rounded_rectangle([64, 64, N - 64, N - 64], radius=200, fill=255)
img.paste(grad, (0, 0), mask)
glass = Image.new("RGBA", (N, N), (0, 0, 0, 0))
ImageDraw.Draw(glass).polygon([(64, 64 + 200), (64 + 200, 64), (N - 64, 64), (N - 64, 420), (64, 700)],
                              fill=(255, 255, 255, 40))
img = Image.alpha_composite(img, Image.composite(glass, Image.new("RGBA", (N, N)), mask))
d = ImageDraw.Draw(img)
d.polygon([(400, 300), (400, 724), (760, 512)], fill=(255, 255, 255, 255))
sgicon.write_ico(sys.argv[1], img)
