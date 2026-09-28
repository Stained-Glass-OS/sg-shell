#!/usr/bin/python3
# Remote Desktop Connection's icon, drawn here: a screen in front, another
# behind it to the top right, and an accent arrow from one to the other. Our
# own drawing, generated at build time.
#
#   sg-mstsc-icon.py OUT.ico
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import sys

from PIL import Image, ImageDraw

DARK = (64, 64, 72, 255)
SCREEN = (200, 178, 236, 255)
FAR = (190, 220, 250, 255)
ACCENT = (112, 48, 192, 255)


def screen(d, u, x, y, w, h, body):
    d.rounded_rectangle([x * u, y * u, (x + w) * u, (y + h) * u], radius=0.8 * u, fill=DARK)
    d.rectangle([(x + 0.9) * u, (y + 0.9) * u, (x + w - 0.9) * u, (y + h - 0.9) * u], fill=body)
    cx = x + w / 2
    d.rectangle([(cx - 1) * u, (y + h) * u, (cx + 1) * u, (y + h + 1.2) * u], fill=DARK)
    d.rounded_rectangle([(cx - 2.6) * u, (y + h + 1.1) * u, (cx + 2.6) * u, (y + h + 1.9) * u],
                        radius=0.3 * u, fill=DARK)


def draw(size):
    s = 4 * size                      # drawn at 4x, filtered down
    u = s / 16.0
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    screen(d, u, 6.5, 0.8, 9, 6.4, FAR)        # the remote one, behind
    screen(d, u, 0.5, 5.5, 10, 7.2, SCREEN)    # this one, in front
    # the arrow: from this screen up to the remote one
    w = max(2, int(1.3 * u))
    d.line([4 * u, 9.5 * u, 11 * u, 4.2 * u], fill=ACCENT, width=w)
    d.polygon([(12.4 * u, 3.1 * u), (8.6 * u, 3.3 * u), (11.2 * u, 6.8 * u)], fill=ACCENT)
    return im.resize((size, size), Image.LANCZOS)


sizes = [16, 24, 32, 48, 64, 256]
big = draw(256)
big.save(sys.argv[1], format="ICO", sizes=[(n, n) for n in sizes],
         append_images=[draw(n) for n in sizes[:-1]])
