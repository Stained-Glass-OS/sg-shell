#!/usr/bin/python3
# Task Manager's icon, drawn here (our own art): a window whose body holds a
# rising graph in the Stained Glass accent. Written as a multi-size .ico at
# build time, so no binary is committed.
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import sgicon  # noqa: E402  (tools/sgicon.py: sizes, reduction, the .ico)

ACCENT = (112, 48, 192, 255)

def draw(size):
    n = size                          # sgicon draws each frame at 4x and reduces it
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = n / 32.0
    # the window
    d.rounded_rectangle([2 * u, 4 * u, 30 * u, 28 * u], radius=3 * u, fill=(255, 255, 255, 255),
                        outline=(96, 96, 96, 255), width=max(1, int(1.2 * u)))
    d.rounded_rectangle([2 * u, 4 * u, 30 * u, 10 * u], radius=3 * u, fill=ACCENT)
    d.rectangle([2 * u, 8 * u, 30 * u, 10 * u], fill=ACCENT)
    # the graph
    pts = [(5 * u, 23 * u), (10 * u, 18 * u), (14 * u, 21 * u), (19 * u, 14 * u), (23 * u, 17 * u), (27 * u, 12 * u)]
    d.polygon(pts + [(27 * u, 25 * u), (5 * u, 25 * u)], fill=(231, 222, 246, 255))
    d.line(pts, fill=ACCENT, width=max(1, int(2 * u)), joint="curve")
    return im


sgicon.write_ico(sys.argv[1], draw)
