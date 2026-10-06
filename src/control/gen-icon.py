#!/usr/bin/python3
"""Control Panel's icon, drawn here: a white panel of three sliders on the
accent purple, the sliders' knobs in the logo's colours.

Drawn at 1024 px and scaled down, so every size is smooth (David
2026-10-04: the Control Panel's icon looked like it needed anti-aliasing --
Start drew a gear of one-pixel lines). Writes an .ico (16-256 px) to the
path given. Our own drawing -- no borrowed artwork.
Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import sgicon  # noqa: E402  (tools/sgicon.py: sizes, reduction, the .ico)

S = 1024
PURPLE = (112, 48, 192, 255)
KNOBS = [(232, 162, 0, 255), (18, 181, 176, 255), (196, 46, 142, 255)]   # gold, teal, magenta

def draw(small):
    """the drawing; small (24 px and under): two thick sliders on a bigger panel"""
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((32, 32, S - 32, S - 32) if small else (64, 64, S - 64, S - 64), radius=150, fill=PURPLE)
    if small:
        d.rounded_rectangle((150, 170, S - 150, S - 170), radius=70, fill=(255, 255, 255, 255))
        rows, track, knob, x0, x1 = ((380, 0.68), (644, 0.32)), 46, 120, 240, S - 240
    else:
        d.rounded_rectangle((200, 220, S - 200, S - 220), radius=60, fill=(255, 255, 255, 255))
        rows, track, knob, x0, x1 = ((360, 0.70), (512, 0.30), (664, 0.55)), 18, 58, 290, S - 290
    for row, (y, at) in enumerate(rows):
        d.rounded_rectangle((x0, y - track, x1, y + track), radius=track, fill=(200, 190, 220, 255))
        kx = x0 + (x1 - x0) * at
        d.rounded_rectangle((x0, y - track, kx, y + track), radius=track, fill=PURPLE)
        d.ellipse((kx - knob, y - knob, kx + knob, y + knob), fill=KNOBS[row])
        if not small:
            d.ellipse((kx - knob, y - knob, kx + knob, y + knob), outline=(255, 255, 255, 255), width=14)
    return img


# 24 px and under: the simpler drawing, two thick sliders
sgicon.write_ico(sys.argv[1], draw(False), small=lambda n: draw(True) if n <= 24 else None)
