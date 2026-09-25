#!/usr/bin/python3
# The font viewer's and the Fonts folder's icons, drawn here: a page with a
# folded corner and a large "A" drawn as strokes, an accent-purple underline;
# the folder's is the same page on a folder. Our own drawing, generated at
# build time.
#
#   gen-icon.py VIEWER.ico FOLDER.ico
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import sys

from PIL import Image, ImageDraw

PURPLE = (112, 48, 192, 255)
GREY = (150, 150, 160, 255)
INK = (40, 40, 48, 255)
FOLDER = (236, 190, 80, 255)
FOLDER_DARK = (210, 160, 50, 255)


def page(d, u, x0, y0, w, h):
    f = w * 0.28
    d.polygon([(x0 * u, y0 * u), ((x0 + w - f) * u, y0 * u), ((x0 + w) * u, (y0 + f) * u),
               ((x0 + w) * u, (y0 + h) * u), (x0 * u, (y0 + h) * u)],
              fill=(255, 255, 255, 255), outline=GREY)
    d.line([((x0 + w - f) * u, y0 * u), ((x0 + w - f) * u, (y0 + f) * u), ((x0 + w) * u, (y0 + f) * u)],
           fill=GREY, width=max(1, int(0.4 * u)))
    # the letter A and an underline in the accent colour
    cx, top, bot = x0 + w / 2, y0 + h * 0.25, y0 + h * 0.72
    lw = max(2, int(w * 0.11 * u))
    d.line([((cx - w * 0.28) * u, bot * u), (cx * u, top * u)], fill=INK, width=lw)
    d.line([(cx * u, top * u), ((cx + w * 0.28) * u, bot * u)], fill=INK, width=lw)
    d.line([((cx - w * 0.15) * u, (y0 + h * 0.56) * u), ((cx + w * 0.15) * u, (y0 + h * 0.56) * u)],
           fill=INK, width=max(2, int(w * 0.08 * u)))
    d.line([((x0 + w * 0.18) * u, (y0 + h * 0.84) * u), ((x0 + w * 0.82) * u, (y0 + h * 0.84) * u)],
           fill=PURPLE, width=max(2, int(w * 0.08 * u)))


def viewer(size):
    s = 4 * size
    u = s / 16.0
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    page(ImageDraw.Draw(im), u, 2.5, 1, 11, 14)
    return im.resize((size, size), Image.LANCZOS)


def folder(size):
    s = 4 * size
    u = s / 16.0
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0.5 * u, 3 * u, 7 * u, 6 * u], radius=0.8 * u, fill=FOLDER_DARK)
    d.rounded_rectangle([0.5 * u, 4.5 * u, 15.5 * u, 14.5 * u], radius=1 * u, fill=FOLDER_DARK)
    page(d, u, 5, 1.5, 8, 10)
    d.rounded_rectangle([0.5 * u, 7 * u, 15.5 * u, 14.5 * u], radius=1 * u, fill=FOLDER)
    return im.resize((size, size), Image.LANCZOS)


sizes = [16, 24, 32, 48, 64, 256]
for fn, out in ((viewer, sys.argv[1]), (folder, sys.argv[2])):
    big = fn(256)
    big.save(out, format="ICO", sizes=[(n, n) for n in sizes], append_images=[fn(n) for n in sizes[:-1]])
