#!/usr/bin/python3
# Report a problem's icon, drawn here: a speech bubble in the accent purple
# with a white "!". Our own drawing, generated at build time.
#
#   gen-icon.py OUT.ico
#
# Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import sys

from PIL import Image, ImageDraw


def draw(size):
    s = 4 * size                      # drawn at 4x, filtered down
    u = s / 16.0
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    purple = (123, 63, 208, 255)
    d.rounded_rectangle([0.8 * u, 1.2 * u, 15.2 * u, 11.6 * u], radius=2.2 * u, fill=purple)
    d.polygon([(3.4 * u, 11.0 * u), (7.0 * u, 11.0 * u), (3.2 * u, 15.0 * u)], fill=purple)   # the tail
    white = (255, 255, 255, 255)
    d.rounded_rectangle([7.1 * u, 3.0 * u, 8.9 * u, 7.9 * u], radius=0.8 * u, fill=white)
    d.ellipse([7.0 * u, 8.6 * u, 9.0 * u, 10.6 * u], fill=white)
    return im.resize((size, size), Image.LANCZOS)


sizes = [16, 24, 32, 48, 64, 256]
big = draw(256)
big.save(sys.argv[1], format="ICO", sizes=[(n, n) for n in sizes],
         append_images=[draw(n) for n in sizes[:-1]])
