#!/usr/bin/python3
# Media Player's icon, drawn here (no borrowed artwork): a rounded tile in the
# Stained Glass purple, a lighter pane like a sheet of glass, and a white play
# triangle. Written as a multi-size .ico for windres.
#
#   sg-media-icon.py OUT.ico
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import sys

from PIL import Image, ImageDraw

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
img.save(sys.argv[1], sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])
