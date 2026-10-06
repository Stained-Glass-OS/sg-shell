#!/usr/bin/python3
"""The network programs' icon (Network Connections, the network flyout):
a monitor with a globe, drawn here -- our own drawing, no borrowed artwork.

Writes an .ico (16-256 px) to the path given.
Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import sgicon  # noqa: E402  (tools/sgicon.py: sizes, reduction, the .ico)

S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
# the monitor
d.rounded_rectangle((96, 150, S - 96, 740), radius=56, fill=(52, 50, 64, 255))
d.rectangle((150, 204, S - 150, 686), fill=(112, 48, 192, 255))
d.rectangle((452, 740, 572, 840), fill=(52, 50, 64, 255))
d.rounded_rectangle((300, 830, S - 300, 900), radius=30, fill=(52, 50, 64, 255))
# a globe on the screen
cx, cy, r = S // 2, 445, 190
w = 22
d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(255, 255, 255, 255), width=w)
d.ellipse((cx - r // 2, cy - r, cx + r // 2, cy + r), outline=(255, 255, 255, 255), width=w)
d.line((cx - r, cy, cx + r, cy), fill=(255, 255, 255, 255), width=w)
d.line((cx, cy - r, cx, cy + r), fill=(255, 255, 255, 255), width=w)
d.line((cx - r + 30, cy - 95, cx + r - 30, cy - 95), fill=(255, 255, 255, 255), width=w)
d.line((cx - r + 30, cy + 95, cx + r - 30, cy + 95), fill=(255, 255, 255, 255), width=w)
sgicon.write_ico(sys.argv[1], img)
