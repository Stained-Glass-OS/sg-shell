#!/usr/bin/python3
"""SG Office's icons, drawn here at build time (no artwork is committed, and
none is anyone else's): for each program -- Documents (blue, lines of text),
Spreadsheets (green, a grid), Presentations (orange, a screen with a chart)
-- an app tile, and a file icon (a folded page with the program's band).

    gen-icons.py OUTDIR     writes OUTDIR/sg-{documents,spreadsheets,presentations}.ico
                            (icon 1: the program, icon 2: its files) and
                            OUTDIR/png/<size>/sg-office-<kind>.png for Linux

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import os
import sys

from PIL import Image, ImageDraw

S = 1024   # drawn at 4x the largest size, then reduced (anti-aliasing)
KINDS = {
    "documents": ((0x2F, 0x6F, 0xD8), (0x1B, 0x4B, 0xA6)),
    "spreadsheets": ((0x23, 0x9A, 0x5E), (0x12, 0x6E, 0x40)),
    "presentations": ((0xEE, 0x74, 0x36), (0xC8, 0x4B, 0x16)),
}
WHITE = (255, 255, 255, 255)


def gradient(size, top, bottom):
    g = Image.new("RGBA", (1, size))
    for y in range(size):
        t = y / (size - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    return g.resize((size, size))


def glyph(d, kind, box, fg, bg):
    """The program's picture inside box (x0, y0, x1, y1)."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    if kind == "documents":
        lh = h / 6.2
        widths = (1.0, 0.86, 1.0, 0.94, 0.62)
        for i, f in enumerate(widths):
            y = y0 + i * lh * 1.25
            d.rounded_rectangle([x0, y, x0 + w * f, y + lh * 0.62], radius=lh * 0.31, fill=fg)
    elif kind == "spreadsheets":
        cols, rows = 3, 4
        t = max(6, int(w * 0.055))
        d.rounded_rectangle([x0, y0, x1, y1], radius=w * 0.08, outline=fg, width=t)
        d.rectangle([x0, y0 + t // 2, x1, y0 + h / rows], fill=fg)          # the header row
        d.rounded_rectangle([x0, y0, x1, y0 + h / rows + t], radius=w * 0.08, fill=fg)
        for c in range(1, cols):
            x = x0 + w * c / cols
            d.line([(x, y0), (x, y1)], fill=fg, width=t)
        for r in range(2, rows):
            y = y0 + h * r / rows
            d.line([(x0, y), (x1, y)], fill=fg, width=t)
    else:
        t = max(6, int(w * 0.05))
        sy1 = y0 + h * 0.74
        d.rounded_rectangle([x0, y0, x1, sy1], radius=w * 0.06, outline=fg, width=t)
        # the chart on the screen
        bw = w * 0.13
        for i, f in enumerate((0.35, 0.6, 0.85)):
            bx = x0 + w * 0.2 + i * bw * 1.7
            d.rectangle([bx, sy1 - t - (sy1 - y0 - 2 * t) * f * 0.8, bx + bw, sy1 - t * 1.6], fill=fg)
        # the stand
        cx = (x0 + x1) / 2
        d.line([(cx, sy1), (cx, y1)], fill=fg, width=t)
        d.line([(cx - w * 0.2, y1 - t / 2), (cx + w * 0.2, y1 - t / 2)], fill=fg, width=t)


def app_icon(kind):
    top, bottom = KINDS[kind]
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    m = 72
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([m, m, S - m, S - m], radius=190, fill=255)
    shadow = Image.new("L", (S, S), 0)
    ImageDraw.Draw(shadow).rounded_rectangle([m + 10, m + 26, S - m + 10, S - m + 26], radius=190, fill=70)
    im.paste((0, 0, 0, 255), (0, 0), shadow)
    im.paste(gradient(S, top, bottom), (0, 0), mask)
    d = ImageDraw.Draw(im)
    # a lighter sheen across the top, as glass catches light
    sheen = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sheen).rounded_rectangle([m, m, S - m, S // 2], radius=190, fill=(255, 255, 255, 34))
    im.alpha_composite(Image.composite(sheen, Image.new("RGBA", (S, S)), mask))
    glyph(d, kind, (m + 170, m + 190, S - m - 170, S - m - 170), WHITE, top)
    return im


def file_icon(kind):
    top, bottom = KINDS[kind]
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x0, y0, x1, y1, fold = 170, 60, S - 170, S - 60, 210
    grey = (0x9A, 0x9A, 0xA4, 255)
    d.polygon([(x0 + 16, y0 + 24), (x1 - fold + 16, y0 + 24), (x1 + 16, y0 + fold + 24), (x1 + 16, y1 + 24),
               (x0 + 16, y1 + 24)], fill=(0, 0, 0, 60))
    page = [(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)]
    d.polygon(page, fill=WHITE)
    d.line(page + [page[0]], fill=grey, width=14)
    d.polygon([(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)], fill=top + (90,))
    d.line([(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)], fill=grey, width=14)
    # the program's tile on the lower left, overlapping the page's edge
    tx0, ty0, tx1, ty1 = x0 - 90, y1 - 520, x0 + 430, y1 - 0
    d.rounded_rectangle([tx0, ty0, tx1, ty1], radius=90, fill=bottom + (255,))
    d.rounded_rectangle([tx0, ty0, tx1, ty1 - 20], radius=90, fill=top + (255,))
    glyph(d, kind, (tx0 + 110, ty0 + 120, tx1 - 110, ty1 - 110), WHITE, top)
    # lines of text on the page, right of the tile
    for i, y in enumerate(range(y0 + 170, y0 + 520, 82)):
        right = x1 - fold - 40 if y < y0 + fold + 20 else x1 - 80
        d.rounded_rectangle([x0 + 80, y, right - (i % 2) * 110, y + 34], radius=17, fill=(0xC4, 0xC4, 0xCE, 255))
    return im


def main(out):
    os.makedirs(out, exist_ok=True)
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    for kind in KINDS:
        app, doc = app_icon(kind), file_icon(kind)
        # two icons in one .ico is not what PIL writes: the program's icon file
        # holds the program; its files' icon is a second .ico, bound as icon 2 by the .rc
        app.resize((256, 256), Image.LANCZOS).save(os.path.join(out, "sg-%s.ico" % kind), format="ICO",
                                                   sizes=[(s, s) for s in sizes])
        doc.resize((256, 256), Image.LANCZOS).save(os.path.join(out, "sg-%s-file.ico" % kind), format="ICO",
                                                   sizes=[(s, s) for s in sizes])
        for s in (16, 24, 32, 48, 64, 128, 256):
            d = os.path.join(out, "png", "%dx%d" % (s, s))
            os.makedirs(d, exist_ok=True)
            app.resize((s, s), Image.LANCZOS).save(os.path.join(d, "sg-office-%s.png" % kind))
    # a sheet of them all, for looking at
    sheet = Image.new("RGBA", (6 * 140, 150), (240, 240, 240, 255))
    for i, kind in enumerate(KINDS):
        sheet.alpha_composite(app_icon(kind).resize((128, 128), Image.LANCZOS), (i * 280 + 6, 11))
        sheet.alpha_composite(file_icon(kind).resize((128, 128), Image.LANCZOS), (i * 280 + 146, 11))
    sheet.save(os.path.join(out, "icons-preview.png"))


if __name__ == "__main__":
    main(sys.argv[1])
