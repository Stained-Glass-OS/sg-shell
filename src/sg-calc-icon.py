#!/usr/bin/python3
"""Calculator's icon, drawn here: a purple tile with a display and keys.

Writes an .ico (16-256 px) to the path given. Our own drawing -- no borrowed
artwork. Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import sys
from PIL import Image, ImageDraw

S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((96, 40, S - 96, S - 40), radius=120, fill=(112, 48, 192, 255))
d.rounded_rectangle((200, 150, S - 200, 330), radius=36, fill=(245, 240, 252, 255))
d.rectangle((560, 222, 760, 258), fill=(112, 48, 192, 255))
keys = 4
gap = 34
kw = (S - 400 - gap * (keys - 1)) / keys
for r in range(4):
    for c in range(keys):
        x = 200 + c * (kw + gap)
        y = 390 + r * (kw * 0.8 + gap)
        color = (255, 255, 255, 255) if not (c == keys - 1 and r == 3) else (220, 196, 250, 255)
        d.rounded_rectangle((x, y, x + kw, y + kw * 0.8), radius=22, fill=color)
img.save(sys.argv[1], sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])
