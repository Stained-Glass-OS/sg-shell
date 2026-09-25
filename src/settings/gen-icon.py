#!/usr/bin/python3
"""Settings' icon, drawn here: a white gear on the accent purple.

Writes an .ico (16-256 px) to the path given. Our own drawing -- no borrowed
artwork. Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import math
import sys
from PIL import Image, ImageDraw

S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((64, 64, S - 64, S - 64), radius=150, fill=(112, 48, 192, 255))
cx = cy = S / 2
teeth = 8
pts = []
for i in range(teeth * 4):
    a = i * 2 * math.pi / (teeth * 4) + math.pi / (teeth * 4)
    r = 330 if (i % 4) in (0, 1) else 250
    pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
d.polygon(pts, fill=(255, 255, 255, 255))
d.ellipse((cx - 250, cy - 250, cx + 250, cy + 250), fill=(255, 255, 255, 255))
d.ellipse((cx - 120, cy - 120, cx + 120, cy + 120), fill=(112, 48, 192, 255))
img.save(sys.argv[1], sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])
