#!/usr/bin/python3
"""Alarms & Clock's icon, drawn here: a purple tile with a white clock face and
hands. Writes an .ico (16-256 px). Our own drawing -- no borrowed artwork.
Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import sgicon  # noqa: E402  (tools/sgicon.py: sizes, reduction, the .ico)

S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((64, 64, S - 64, S - 64), radius=150, fill=(112, 48, 192, 255))
d.ellipse((200, 200, S - 200, S - 200), fill=(255, 255, 255, 255))
d.ellipse((250, 250, S - 250, S - 250), fill=(112, 48, 192, 255))
d.line([(512, 512), (512, 330)], fill=(255, 255, 255, 255), width=56)
d.line([(512, 512), (640, 600)], fill=(255, 255, 255, 255), width=56)
d.ellipse((480, 480, 544, 544), fill=(255, 255, 255, 255))
sgicon.write_ico(sys.argv[1], img)
