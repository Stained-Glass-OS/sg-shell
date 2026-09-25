#!/usr/bin/python3
# The Sticky Notes icon, drawn here: a yellow note with a darker strip along
# its top, three lines of writing and a turned-up corner. Our own drawing,
# generated at build time.
#
#   gen-icon.py OUT.ico
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import sys

from PIL import Image, ImageDraw


def draw(size):
    s = 4 * size                      # drawn at 4x, filtered down
    u = s / 16.0
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    body, strip, ink, fold = (0xFF, 0xE6, 0x6B, 255), (0xF2, 0xC9, 0x2E, 255), (0x70, 0x30, 0xC0, 255), (0xE0, 0xB8, 0x2A, 255)
    d.polygon([(2 * u, 2 * u), (14 * u, 2 * u), (14 * u, 10.5 * u), (10.5 * u, 14 * u), (2 * u, 14 * u)], fill=body)
    d.rectangle([2 * u, 2 * u, 14 * u, 4.5 * u], fill=strip)
    d.polygon([(14 * u, 10.5 * u), (10.5 * u, 10.5 * u), (10.5 * u, 14 * u)], fill=fold)
    for k, w in enumerate((8.5, 7.5, 5.0)):
        y = (6.5 + 2.2 * k) * u
        d.rounded_rectangle([4 * u, y, (4 + w) * u, y + 0.9 * u], radius=0.45 * u, fill=ink)
    return im.resize((size, size), Image.LANCZOS)


sizes = [16, 24, 32, 48, 64, 256]
big = draw(256)
big.save(sys.argv[1], format="ICO", sizes=[(n, n) for n in sizes],
         append_images=[draw(n) for n in sizes[:-1]])
