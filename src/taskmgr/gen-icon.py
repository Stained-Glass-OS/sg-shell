#!/usr/bin/python3
# Task Manager's icon, drawn here (our own art): a window whose body holds a
# rising graph in the Stained Glass accent. Written as a multi-size .ico at
# build time, so no binary is committed.
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import sys
from PIL import Image, ImageDraw

ACCENT = (112, 48, 192, 255)
SS = 4

def draw(size):
    n = size * SS
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
    return im.resize((size, size), Image.LANCZOS)

sizes = [16, 24, 32, 48, 64, 256]
imgs = [draw(s) for s in sizes]
imgs[-1].save(sys.argv[1], format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
