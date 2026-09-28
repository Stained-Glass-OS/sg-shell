#!/usr/bin/env python3
"""sg-icons-font.py OUT.ttf -- Stained Glass Icons, the shell's icon font.

Programs written for Windows 10 and 11 draw their caption buttons and a few
other controls with characters of the system's icon font -- Firefox's
minimize, maximize, restore and close are U+E921, U+E922, U+E923 and U+E8BB
("Segoe MDL2 Assets", "Segoe Fluent Icons"). That font is not ours to ship,
and without it those buttons were empty squares. This one is drawn here, from
lines and boxes, at the code points programs ask for; theme/52-sg-fonts.reg
maps the two font names to it.

The glyphs are original: plain geometric strokes, about 1 px at the 10 px
the caption buttons use (a 1000-unit em, 100-unit strokes).
"""
import math
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

UPM = 1000
ASC, DESC = 875, -125
W = 100           # stroke
LO, HI = DESC, ASC  # the em box, vertically
MID = (LO + HI) // 2


def rect(pen, x0, y0, x1, y1):
    """a filled rectangle, clockwise"""
    pen.moveTo((x0, y0)); pen.lineTo((x0, y1)); pen.lineTo((x1, y1)); pen.lineTo((x1, y0)); pen.closePath()


def rect_ccw(pen, x0, y0, x1, y1):
    pen.moveTo((x0, y0)); pen.lineTo((x1, y0)); pen.lineTo((x1, y1)); pen.lineTo((x0, y1)); pen.closePath()


def frame(pen, x0, y0, x1, y1, w=W):
    """a square outline: the outer contour, and the hole the other way round"""
    rect(pen, x0, y0, x1, y1)
    rect_ccw(pen, x0 + w, y0 + w, x1 - w, y1 - w)


def line(pen, x0, y0, x1, y1, w=W):
    """a straight stroke from (x0, y0) to (x1, y1), square ends"""
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy) or 1
    ox, oy = -dy / n * w / 2, dx / n * w / 2
    pts = [(x0 + ox, y0 + oy), (x1 + ox, y1 + oy), (x1 - ox, y1 - oy), (x0 - ox, y0 - oy)]
    # clockwise
    area = sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(pts, pts[1:] + pts[:1]))
    if area > 0:
        pts.reverse()
    pen.moveTo(tuple(map(round, pts[0])))
    for p in pts[1:]:
        pen.lineTo(tuple(map(round, p)))
    pen.closePath()


def chevron(pen, direction):
    """a V: down, up, left or right"""
    a, b = 250, 750
    if direction == "down":
        line(pen, a, MID + 125, 500, MID - 125); line(pen, 500, MID - 125, b, MID + 125)
    elif direction == "up":
        line(pen, a, MID - 125, 500, MID + 125); line(pen, 500, MID + 125, b, MID - 125)
    elif direction == "left":
        line(pen, 625, MID + 250, 375, MID); line(pen, 375, MID, 625, MID - 250)
    else:
        line(pen, 375, MID + 250, 625, MID); line(pen, 625, MID, 375, MID - 250)


def g_minimize(pen):
    rect(pen, 0, MID - W // 2, 1000, MID + W // 2)


def g_maximize(pen):
    frame(pen, 0, LO, 1000, HI)


def g_restore(pen):
    # the window behind: only its top and right edges show
    rect(pen, 200, HI - W, 1000, HI)
    rect(pen, 1000 - W, LO + 200, 1000, HI)
    # the window in front
    frame(pen, 0, LO, 800, HI - 200)


def g_close(pen):
    line(pen, 0, HI, 1000, LO)
    line(pen, 0, LO, 1000, HI)


def g_add(pen):
    rect(pen, 0, MID - W // 2, 1000, MID + W // 2)
    rect(pen, 500 - W // 2, LO, 500 + W // 2, HI)


def g_hamburger(pen):
    for y in (MID - 300, MID, MID + 300):
        rect(pen, 0, y - W // 2, 1000, y + W // 2)


def g_more(pen):
    for x in (150, 500, 850):
        rect(pen, x - 60, MID - 60, x + 60, MID + 60)


def g_check(pen):
    line(pen, 50, MID, 350, MID - 300)
    line(pen, 350, MID - 300, 950, MID + 350)


def g_back(pen):
    rect(pen, 50, MID - W // 2, 1000, MID + W // 2)
    line(pen, 450, MID + 400, 50, MID); line(pen, 50, MID, 450, MID - 400)


def g_forward(pen):
    rect(pen, 0, MID - W // 2, 950, MID + W // 2)
    line(pen, 550, MID + 400, 950, MID); line(pen, 950, MID, 550, MID - 400)


def g_search(pen):
    # a ring (as a square-ish octagon) and a handle
    cx, cy, r = 600, MID + 125, 330
    outer = [(cx + r * math.cos(t), cy + r * math.sin(t)) for t in [i * math.pi / 8 for i in range(16)]]
    inner = [(cx + (r - W) * math.cos(t), cy + (r - W) * math.sin(t)) for t in [i * math.pi / 8 for i in range(16)]]
    pen.moveTo(tuple(map(round, outer[0])))
    for p in reversed(outer[1:]):
        pen.lineTo(tuple(map(round, p)))
    pen.closePath()
    pen.moveTo(tuple(map(round, inner[0])))
    for p in inner[1:]:
        pen.lineTo(tuple(map(round, p)))
    pen.closePath()
    line(pen, 30, LO + 30, cx - r * 0.7, cy - r * 0.7)


def g_refresh(pen):
    # three quarters of a ring and an arrowhead
    cx, cy, r = 500, MID, 420
    steps = [i * math.pi / 12 for i in range(3, 22)]
    outer = [(cx + r * math.cos(t), cy + r * math.sin(t)) for t in steps]
    inner = [(cx + (r - W) * math.cos(t), cy + (r - W) * math.sin(t)) for t in steps]
    pts = outer + list(reversed(inner))
    area = sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(pts, pts[1:] + pts[:1]))
    if area > 0:
        pts.reverse()
    pen.moveTo(tuple(map(round, pts[0])))
    for p in pts[1:]:
        pen.lineTo(tuple(map(round, p)))
    pen.closePath()
    ex, ey = cx + r * math.cos(steps[0]), cy + r * math.sin(steps[0])
    line(pen, ex - 30, ey + 10, ex + 250, ey + 10)
    line(pen, ex + 10, ey + 10, ex + 10, ey - 270)


GLYPHS = {
    # caption buttons, and their high-contrast forms (the same here)
    0xE921: ("chromeMinimize", g_minimize),
    0xE922: ("chromeMaximize", g_maximize),
    0xE923: ("chromeRestore", g_restore),
    0xE8BB: ("chromeClose", g_close),
    0xEF2D: ("chromeMinimizeContrast", g_minimize),
    0xEF2E: ("chromeMaximizeContrast", g_maximize),
    0xEF2F: ("chromeRestoreContrast", g_restore),
    0xEF2C: ("chromeCloseContrast", g_close),
    # common controls
    0xE70D: ("chevronDown", lambda p: chevron(p, "down")),
    0xE70E: ("chevronUp", lambda p: chevron(p, "up")),
    0xE76B: ("chevronLeft", lambda p: chevron(p, "left")),
    0xE76C: ("chevronRight", lambda p: chevron(p, "right")),
    0xE711: ("cancel", g_close),
    0xE710: ("add", g_add),
    0xE700: ("globalNav", g_hamburger),
    0xE712: ("more", g_more),
    0xE73E: ("checkMark", g_check),
    0xE72B: ("back", g_back),
    0xE72A: ("forward", g_forward),
    0xE721: ("search", g_search),
    0xE72C: ("refresh", g_refresh),
}


def build(out):
    order = [".notdef", "space"] + [name for name, _ in GLYPHS.values()]
    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(order)
    cmap = {0x20: "space"}
    cmap.update({cp: name for cp, (name, _) in GLYPHS.items()})
    fb.setupCharacterMap(cmap)
    glyphs, metrics = {}, {}
    pen = TTGlyphPen(None)
    rect(pen, 100, 0, 900, 700)
    rect_ccw(pen, 200, 100, 800, 600)
    glyphs[".notdef"] = pen.glyph(); metrics[".notdef"] = (1000, 100)
    glyphs["space"] = TTGlyphPen(None).glyph(); metrics["space"] = (1000, 0)
    for name, draw in GLYPHS.values():
        pen = TTGlyphPen(None)
        draw(pen)
        glyphs[name] = pen.glyph()
        metrics[name] = (1000, 0)
    fb.setupGlyf(glyphs)
    for name in glyphs:
        g = glyphs[name]
        if hasattr(g, "recalcBounds"):
            g.recalcBounds(fb.font["glyf"])
        metrics[name] = (1000, getattr(g, "xMin", 0) or 0)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=ASC, descent=DESC)
    fb.setupNameTable({
        "familyName": "Stained Glass Icons",
        "styleName": "Regular",
        "uniqueFontIdentifier": "StainedGlassIcons-Regular-1.0",
        "fullName": "Stained Glass Icons",
        "psName": "StainedGlassIcons-Regular",
        "version": "Version 1.0",
        "copyright": "Copyright 2026 Stained Glass OS contributors",
        "licenseDescription": "SIL Open Font License 1.1",
    })
    fb.setupOS2(sTypoAscender=ASC, sTypoDescender=DESC, sTypoLineGap=0,
                usWinAscent=ASC, usWinDescent=-DESC, achVendID="SGOS",
                ulUnicodeRange1=0, ulCodePageRange1=1)
    fb.setupPost()
    fb.setupHead(unitsPerEm=UPM)
    fb.save(out)


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "StainedGlassIcons.ttf")
