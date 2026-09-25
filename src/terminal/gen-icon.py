#!/usr/bin/python3
"""Terminal's icon, drawn here: a dark window with a purple prompt chevron and
a cursor bar. Writes an .ico (16-256 px). Our own drawing -- no borrowed
artwork. Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import sys
from PIL import Image, ImageDraw

S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((64, 128, S - 64, S - 128), radius=110, fill=(29, 26, 38, 255))
d.rounded_rectangle((64, 128, S - 64, 290), radius=110, fill=(58, 50, 78, 255))
d.rectangle((64, 220, S - 64, 290), fill=(58, 50, 78, 255))
d.line([(230, 440), (400, 560), (230, 680)], fill=(155, 108, 240, 255), width=70, joint="curve")
d.rounded_rectangle((470, 640, 760, 700), radius=20, fill=(232, 228, 240, 255))
img.save(sys.argv[1], sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])
