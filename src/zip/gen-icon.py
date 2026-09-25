#!/usr/bin/python3
# The Compressed (zipped) Folder icon, drawn here: an amber folder with a
# zipper down its middle. Our own drawing, generated at build time.
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
    back, front, zip_ = (0xD9, 0x9A, 0x2B, 255), (0xF8, 0xCE, 0x62, 255), (0x5A, 0x3A, 0x10, 255)
    d.rounded_rectangle([1 * u, 2 * u, 7 * u, 6 * u], radius=u * 0.6, fill=back)
    d.rounded_rectangle([1 * u, 3.5 * u, 15 * u, 14 * u], radius=u * 0.6, fill=back)
    d.rounded_rectangle([1 * u, 5 * u, 15 * u, 14 * u], radius=u * 0.6, fill=front)
    for k in range(4):
        x = 7 * u + (k % 2) * u
        d.rectangle([x, (5 + 2 * k) * u, x + u, (6 + 2 * k) * u], fill=zip_)
    d.rounded_rectangle([6.8 * u, 12 * u, 9.2 * u, 14 * u], radius=u * 0.4, fill=zip_)
    return im.resize((size, size), Image.LANCZOS)


sizes = [16, 24, 32, 48, 64, 256]
big = draw(256)
big.save(sys.argv[1], format="ICO", sizes=[(n, n) for n in sizes],
         append_images=[draw(n) for n in sizes[:-1]])
