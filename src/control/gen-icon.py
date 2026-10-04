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
import sys
from PIL import Image, ImageDraw

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


big, little = draw(False), draw(True)
# each size from the big drawing, smoothly
sizes = [16, 20, 24, 32, 40, 48, 64, 96, 256]
frames = [(little if n <= 24 else big).resize((n, n), Image.LANCZOS) for n in sizes]
frames[-1].save(sys.argv[1], sizes=[(n, n) for n in sizes], append_images=frames[:-1])
