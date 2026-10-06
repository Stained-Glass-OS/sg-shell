#!/usr/bin/python3
# Magnifier's icon, drawn here: a magnifying glass -- a ring with a light
# purple lens and a highlight, and a dark handle. Our own drawing, generated
# at build time.
#
#   gen-icon.py OUT.ico
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import sgicon  # noqa: E402  (tools/sgicon.py: sizes, reduction, the .ico)


def draw(size):
    s = size                          # sgicon draws each frame at 4x and reduces it
    u = s / 16.0
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    purple, lens, ink = (112, 48, 192, 255), (214, 196, 245, 255), (40, 40, 48, 255)
    d.line([9.6 * u, 9.6 * u, 14.6 * u, 14.6 * u], fill=ink, width=int(2.4 * u))
    d.ellipse([1 * u, 1 * u, 11 * u, 11 * u], fill=purple)
    d.ellipse([2.4 * u, 2.4 * u, 9.6 * u, 9.6 * u], fill=lens)
    d.arc([3.4 * u, 3.4 * u, 8.6 * u, 8.6 * u], 190, 260, fill=(255, 255, 255, 255), width=max(1, int(0.8 * u)))
    return im


sgicon.write_ico(sys.argv[1], draw)
