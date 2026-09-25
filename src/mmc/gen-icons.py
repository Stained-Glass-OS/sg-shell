#!/usr/bin/python3
# The administrative consoles' pictures, drawn here (our own art): one strip of
# 16x16 (and one of 32x32) images in the order of mmc.h's IC_* enum, written as
# 32-bit BMPs with alpha for an ILC_COLOR32 image list, plus the console's
# program icon. Generated at build time, so no binary is committed.
#
#   gen-icons.py OUT16.bmp OUT32.bmp OUT.ico [eventvwr.ico msinfo.ico resmon.ico cleanmgr.ico]
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import struct
import sys
from PIL import Image, ImageDraw

ACCENT = (112, 48, 192, 255)
ACCENT_LT = (200, 178, 236, 255)
DARK = (64, 64, 72, 255)
GREY = (128, 128, 136, 255)
LIGHT = (236, 236, 240, 255)
WHITE = (255, 255, 255, 255)
BLUE = (40, 110, 200, 255)
GREEN = (30, 150, 70, 255)
RED = (205, 40, 40, 255)
AMBER = (240, 180, 20, 255)
SS = 8


class Pen:
    def __init__(self, d, n):
        self.d, self.u = d, n / 16.0

    def p(self, *xy):
        return [v * self.u for v in xy]

    def rect(self, x0, y0, x1, y1, fill=None, outline=None, w=1.0, r=0):
        box = self.p(x0, y0, x1, y1)
        width = max(1, int(w * self.u)) if outline else 0
        if r:
            self.d.rounded_rectangle(box, radius=r * self.u, fill=fill, outline=outline, width=width)
        else:
            self.d.rectangle(box, fill=fill, outline=outline, width=width)

    def ellipse(self, x0, y0, x1, y1, fill=None, outline=None, w=1.0):
        self.d.ellipse(self.p(x0, y0, x1, y1), fill=fill, outline=outline,
                       width=max(1, int(w * self.u)) if outline else 0)

    def poly(self, pts, fill=None, outline=None):
        self.d.polygon([(x * self.u, y * self.u) for x, y in pts], fill=fill, outline=outline)

    def line(self, pts, fill, w=1.2):
        self.d.line([(x * self.u, y * self.u) for x, y in pts], fill=fill, width=max(1, int(w * self.u)),
                    joint="curve")

    def arc(self, x0, y0, x1, y1, a0, a1, fill, w=1.2):
        self.d.arc(self.p(x0, y0, x1, y1), a0, a1, fill=fill, width=max(1, int(w * self.u)))


def monitor(p, body=ACCENT_LT):
    p.rect(1.5, 2.5, 14.5, 11, fill=DARK, r=1)
    p.rect(2.5, 3.5, 13.5, 10, fill=body)
    p.rect(6.5, 11, 9.5, 13, fill=GREY)
    p.rect(4.5, 13, 11.5, 14.2, fill=DARK, r=0.5)


def gear(p, cx, cy, r, col=ACCENT, hole=WHITE):
    import math
    pts = []
    for i in range(16):
        a = i * math.pi / 8
        rr = r if i % 2 == 0 else r * 0.72
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    p.poly(pts, fill=col)
    p.ellipse(cx - r * 0.62, cy - r * 0.62, cx + r * 0.62, cy + r * 0.62, fill=col)
    p.ellipse(cx - r * 0.3, cy - r * 0.3, cx + r * 0.3, cy + r * 0.3, fill=hole)


def doc(p, lines=True):
    p.poly([(3, 1.5), (10, 1.5), (13, 4.5), (13, 14.5), (3, 14.5)], fill=WHITE, outline=DARK)
    p.poly([(10, 1.5), (10, 4.5), (13, 4.5)], fill=LIGHT, outline=DARK)
    if lines:
        for y in (7, 9, 11):
            p.line([(5, y), (11, y)], GREY, 0.9)


def disk(p, col=GREY):
    p.rect(1.5, 4.5, 14.5, 11.5, fill=col, outline=DARK, r=1.5)
    p.rect(2.5, 5.5, 13.5, 8, fill=LIGHT, r=1)
    p.ellipse(11, 9, 12.6, 10.6, fill=GREEN)


def folder(p, col=AMBER):
    p.poly([(1.5, 3.5), (6, 3.5), (7.5, 5), (14.5, 5), (14.5, 13.5), (1.5, 13.5)], fill=col, outline=(180, 130, 10, 255))
    p.rect(1.5, 6.5, 14.5, 13.5, fill=(250, 205, 70, 255))


def person(p, cx, col=ACCENT, s=1.0):
    p.ellipse(cx - 2.2 * s, 2 * s + 1, cx + 2.2 * s, 6.4 * s + 1, fill=col)
    p.d.pieslice(p.p(cx - 4.2 * s, 8 * s, cx + 4.2 * s, 8 * s + 11 * s), 180, 360, fill=col)


def diamond(p, cx=8, cy=8, r=6.5):
    p.poly([(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)], fill=ACCENT)
    p.poly([(cx, cy - r), (cx + r, cy), (cx, cy)], fill=(150, 90, 220, 255))
    p.poly([(cx - r, cy), (cx, cy), (cx, cy + r)], fill=(80, 30, 150, 255))
    p.poly([(cx, cy), (cx + r, cy), (cx, cy + r)], fill=(120, 60, 200, 255))


def arrow(p, direction, col=ACCENT):
    if direction == "left":
        p.poly([(2, 8), (8, 2.5), (8, 5.5), (14, 5.5), (14, 10.5), (8, 10.5), (8, 13.5)], fill=col)
    elif direction == "right":
        p.poly([(14, 8), (8, 2.5), (8, 5.5), (2, 5.5), (2, 10.5), (8, 10.5), (8, 13.5)], fill=col)
    else:
        folder(p)
        p.poly([(10.5, 5), (14.5, 9), (12, 9), (12, 13), (9, 13), (9, 9), (6.5, 9)], fill=ACCENT)


def draw(name, p):
    if name == "COMPUTER":
        monitor(p)
    elif name == "SERVICES":
        gear(p, 6, 6, 5)
        gear(p, 11, 11, 4.2, col=GREY)
    elif name == "SERVICE":
        gear(p, 8, 8, 6.5)
    elif name == "EVENTS":
        doc(p)
        p.ellipse(7.5, 7.5, 15, 15, fill=AMBER)
        p.line([(11.25, 9), (11.25, 12)], DARK, 1.3)
        p.ellipse(10.7, 12.7, 11.8, 13.8, fill=DARK)
    elif name == "LOG":
        doc(p)
    elif name == "ERROR":
        p.ellipse(1, 1, 15, 15, fill=RED)
        p.line([(5, 5), (11, 11)], WHITE, 1.8)
        p.line([(11, 5), (5, 11)], WHITE, 1.8)
    elif name == "WARNING":
        p.poly([(8, 1), (15.3, 14.5), (0.7, 14.5)], fill=AMBER)
        p.line([(8, 5.5), (8, 10)], DARK, 1.6)
        p.ellipse(7.1, 11.3, 8.9, 13.1, fill=DARK)
    elif name == "INFO":
        p.ellipse(1, 1, 15, 15, fill=BLUE)
        p.ellipse(7, 3.5, 9, 5.5, fill=WHITE)
        p.line([(8, 7), (8, 12.5)], WHITE, 1.8)
    elif name == "DEVMGR":
        monitor(p)
        gear(p, 11.5, 11.5, 4, col=ACCENT)
    elif name == "DEVICE":
        p.rect(2.5, 4, 13.5, 12, fill=ACCENT_LT, outline=DARK, r=1)
        for x in (4, 7, 10):
            p.line([(x + 1, 2), (x + 1, 4)], DARK, 1)
            p.line([(x + 1, 12), (x + 1, 14)], DARK, 1)
    elif name == "DISPLAY":
        p.rect(1, 4, 15, 12, fill=GREEN, outline=DARK, r=1)
        p.rect(3, 5.5, 9, 10.5, fill=DARK)
        p.rect(10, 6, 14, 7.5, fill=AMBER)
        p.rect(2, 12, 12, 13.5, fill=AMBER)
    elif name == "NETWORK":
        monitor(p, body=(190, 220, 250, 255))
        p.line([(8, 14), (8, 15.5), (14, 15.5)], BLUE, 1.2)
    elif name == "DISK":
        disk(p)
    elif name == "SOUND":
        p.poly([(2, 6), (5, 6), (9, 2.5), (9, 13.5), (5, 10), (2, 10)], fill=DARK)
        p.arc(8, 4, 13, 12, -60, 60, ACCENT, 1.3)
        p.arc(8, 1.5, 15.5, 14.5, -60, 60, ACCENT, 1.3)
    elif name == "USB":
        p.line([(8, 1.5), (8, 14.5)], DARK, 1.5)
        p.poly([(8, 0.5), (10, 3), (6, 3)], fill=DARK)
        p.line([(8, 9), (4, 6.5), (4, 5)], DARK, 1.3)
        p.line([(8, 11), (12, 8.5), (12, 7)], DARK, 1.3)
        p.ellipse(3, 3.5, 5, 5.5, fill=DARK)
        p.rect(11, 5.5, 13, 7.5, fill=DARK)
        p.ellipse(6.5, 13, 9.5, 16, fill=DARK)
    elif name == "KEYBOARD":
        p.rect(0.5, 4, 15.5, 12.5, fill=LIGHT, outline=DARK, r=1)
        for y in (5.5, 7.5):
            for x in range(2, 14, 2):
                p.rect(x, y, x + 1.2, y + 1.2, fill=DARK)
        p.rect(4, 10, 12, 11.2, fill=DARK)
    elif name == "MOUSE":
        p.ellipse(4, 2, 12, 15, fill=LIGHT, outline=DARK)
        p.line([(8, 2), (8, 7)], DARK, 1)
        p.line([(4.3, 7), (11.7, 7)], DARK, 1)
    elif name == "CPU":
        p.rect(3, 3, 13, 13, fill=GREY, r=1)
        p.rect(5, 5, 11, 11, fill=ACCENT)
        for i in (4.5, 7.5, 10.5):
            for (a, b, c, d) in ((i, 1, i, 3), (i, 13, i, 15), (1, i, 3, i), (13, i, 15, i)):
                p.line([(a, b), (c, d)], DARK, 0.9)
    elif name == "SYSTEM":
        p.rect(2, 2, 14, 14, fill=LIGHT, outline=DARK, r=1)
        gear(p, 8, 8, 4.5, col=GREY)
    elif name == "DISKMGMT":
        disk(p)
        p.ellipse(9, 1, 15, 7, fill=ACCENT)
        p.line([(12, 2.5), (12, 5.5)], WHITE, 1)
        p.line([(10.5, 4), (13.5, 4)], WHITE, 1)
    elif name == "USERS":
        person(p, 10.5, col=GREY, s=0.85)
        person(p, 6, col=ACCENT)
    elif name == "USER":
        person(p, 8)
    elif name == "GROUP":
        person(p, 11, col=GREY, s=0.8)
        person(p, 5, col=GREY, s=0.8)
        person(p, 8, col=ACCENT, s=0.9)
    elif name == "SHARE":
        folder(p)
        person(p, 11.5, col=BLUE, s=0.6)
    elif name == "FOLDER":
        folder(p)
    elif name == "SGLOGO":
        diamond(p)
    elif name == "AUDIT_OK":
        p.ellipse(1.5, 4, 8.5, 11, outline=AMBER, w=1.8)
        p.line([(8, 7.5), (15, 7.5), (15, 10)], AMBER, 1.8)
        p.line([(12.5, 7.5), (12.5, 10)], AMBER, 1.5)
    elif name == "AUDIT_FAIL":
        p.rect(3, 7, 13, 14.5, fill=AMBER, r=1)
        p.arc(4.5, 1.5, 11.5, 11, 180, 360, GREY, 1.8)
        p.ellipse(7, 9.5, 9, 11.5, fill=DARK)
    elif name == "NODRIVER":
        p.poly([(8, 1), (15.3, 14.5), (0.7, 14.5)], fill=AMBER, outline=DARK)
        p.line([(8, 5.5), (8, 10)], DARK, 1.6)
        p.ellipse(7.1, 11.3, 8.9, 13.1, fill=DARK)
    elif name == "CDROM":
        p.ellipse(1.5, 1.5, 14.5, 14.5, fill=(220, 220, 230, 255), outline=GREY)
        p.ellipse(6, 6, 10, 10, fill=WHITE, outline=GREY)
        p.arc(3, 3, 13, 13, 200, 250, ACCENT, 1.2)
    elif name == "BATTERY":
        p.rect(1, 4.5, 13.5, 11.5, fill=WHITE, outline=DARK, r=1)
        p.rect(13.5, 6.5, 15, 9.5, fill=DARK)
        p.rect(2.5, 6, 10, 10, fill=GREEN)
    elif name == "CAMERA":
        p.rect(1, 4.5, 15, 13, fill=DARK, r=1.5)
        p.rect(5, 3, 9.5, 5, fill=DARK)
        p.ellipse(5, 6, 11, 12, fill=LIGHT)
        p.ellipse(6.8, 7.8, 9.2, 10.2, fill=BLUE)
    elif name == "BLUETOOTH":
        p.ellipse(3, 0.5, 13, 15.5, fill=BLUE)
        p.line([(5.5, 5), (10.5, 10.5), (8, 13), (8, 3), (10.5, 5.5), (5.5, 11)], WHITE, 1.1)
    elif name == "PRINTER":
        p.rect(4, 1.5, 12, 6, fill=WHITE, outline=DARK)
        p.rect(1.5, 6, 14.5, 12, fill=GREY, r=1)
        p.rect(4, 10, 12, 14.5, fill=WHITE, outline=DARK)
    elif name == "MONITOR":
        monitor(p, body=(210, 230, 250, 255))
    elif name == "STORAGE":
        p.rect(1.5, 3, 14.5, 13, fill=GREY, r=1)
        for y in (5, 8, 11):
            p.line([(3, y), (10, y)], LIGHT, 1)
        p.ellipse(11.5, 4, 13.3, 5.8, fill=GREEN)
    elif name == "HID":
        p.rect(3, 2, 13, 14, fill=LIGHT, outline=DARK, r=2)
        p.ellipse(5.5, 4.5, 10.5, 9.5, fill=ACCENT)
        p.rect(5.5, 11, 10.5, 12.2, fill=DARK)
    elif name == "TOOLS":
        folder(p)
        gear(p, 11.5, 10.5, 3.5, col=GREY, hole=(250, 205, 70, 255))
    elif name == "STORAGEFOLDER":
        folder(p)
        p.rect(8, 8.5, 14, 12.5, fill=GREY, r=0.6)
        p.ellipse(12, 10, 13.2, 11.2, fill=GREEN)
    elif name == "SESSION":
        monitor(p)
        person(p, 8, col=ACCENT, s=0.55)
    elif name == "BACK":
        arrow(p, "left")
    elif name == "FORWARD":
        arrow(p, "right")
    elif name == "UP":
        arrow(p, "up")
    elif name == "TREE":
        p.rect(1.5, 2, 14.5, 14, fill=WHITE, outline=DARK)
        p.rect(1.5, 2, 6, 14, fill=ACCENT_LT, outline=DARK)
        for y in (5, 8, 11):
            p.line([(2.8, y), (4.8, y)], ACCENT, 1)
    elif name == "REFRESH":
        p.arc(2, 2, 14, 14, 20, 320, GREEN, 1.8)
        p.poly([(10.5, 1), (15, 3.5), (11, 6.5)], fill=GREEN)
    elif name == "PROPS":
        p.rect(2, 1.5, 14, 14.5, fill=WHITE, outline=DARK, r=1)
        for y in (5, 8, 11):
            p.rect(4, y - 0.6, 5.2, y + 0.6, fill=ACCENT)
            p.line([(6.5, y), (12, y)], GREY, 1)
    elif name == "HELP":
        p.ellipse(1, 1, 15, 15, fill=BLUE)
        p.arc(5, 3.5, 11, 9, 180, 450, WHITE, 1.6)
        p.line([(8, 9), (8, 10.3)], WHITE, 1.6)
        p.ellipse(7.1, 11.5, 8.9, 13.3, fill=WHITE)
    elif name == "START":
        p.poly([(4, 2), (13.5, 8), (4, 14)], fill=GREEN)
    elif name == "STOP":
        p.rect(3, 3, 13, 13, fill=RED, r=1)
    elif name == "PAUSE":
        p.rect(3.5, 2.5, 6.5, 13.5, fill=BLUE, r=0.5)
        p.rect(9.5, 2.5, 12.5, 13.5, fill=BLUE, r=0.5)
    elif name == "RESTART":
        p.rect(1.5, 4, 5.5, 12, fill=RED, r=0.5)
        p.poly([(7.5, 2.5), (15, 8), (7.5, 13.5)], fill=GREEN)
    elif name == "EXPORT":
        doc(p, lines=False)
        p.poly([(6, 7), (10, 7), (10, 5), (14.5, 9), (10, 13), (10, 11), (6, 11)], fill=ACCENT)
    elif name == "FILTER":
        p.poly([(1.5, 2), (14.5, 2), (9.5, 8), (9.5, 14), (6.5, 12), (6.5, 8)], fill=ACCENT)
    elif name == "CLEAR":
        doc(p)
        p.line([(4, 13), (13, 4)], RED, 1.8)
    elif name == "SCAN":
        monitor(p)
        p.ellipse(8, 7, 13, 12, outline=ACCENT, w=1.5)
        p.line([(12.3, 11.3), (15, 14)], ACCENT, 1.8)
    elif name == "ACTIONS":
        p.rect(1.5, 2, 14.5, 14, fill=WHITE, outline=DARK)
        p.rect(10, 2, 14.5, 14, fill=ACCENT_LT, outline=DARK)


NAMES = ("COMPUTER SERVICES SERVICE EVENTS LOG ERROR WARNING INFO "
         "DEVMGR DEVICE DISPLAY NETWORK DISK SOUND USB KEYBOARD "
         "MOUSE CPU SYSTEM DISKMGMT USERS USER GROUP SHARE "
         "FOLDER SGLOGO AUDIT_OK AUDIT_FAIL NODRIVER CDROM BATTERY CAMERA "
         "BLUETOOTH PRINTER MONITOR STORAGE HID TOOLS STORAGEFOLDER SESSION "
         "BACK FORWARD UP TREE REFRESH PROPS HELP START "
         "STOP PAUSE RESTART EXPORT FILTER CLEAR SCAN ACTIONS").split()


def render(name, size):
    n = size * SS
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    draw(name, Pen(ImageDraw.Draw(im), n))
    return im.resize((size, size), Image.LANCZOS)


def write_bmp(path, im):
    """A bottom-up 32-bit BI_RGB BMP whose fourth byte is straight alpha."""
    w, h = im.size
    px = im.load()
    rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b, a = px[x, y]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    data = b"".join(rows)
    header = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, len(data), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", 14 + 40 + len(data), 0, 0, 54))
        f.write(header)
        f.write(data)


def strip(size):
    im = Image.new("RGBA", (size * len(NAMES), size), (0, 0, 0, 0))
    for i, name in enumerate(NAMES):
        im.paste(render(name, size), (i * size, 0))
    return im


def program_icon(path, names):
    sizes = [16, 24, 32, 48, 64, 256]
    imgs = []
    for s in sizes:
        im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        for nm in names:
            layer = render(nm, s)
            im.alpha_composite(layer)
        imgs.append(im)
    imgs[-1].save(path, format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])


def check_enum():
    """The strip's order is mmc.h's IC_* enum: refuse to build a mismatch."""
    import os
    import re
    h = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "mmc.h")).read()
    body = re.search(r"enum\s*\{\s*(IC_COMPUTER.*?)IC_COUNT", h, re.S).group(1)
    enum = re.findall(r"IC_(\w+)", body)
    if enum != NAMES:
        sys.exit("gen-icons.py: NAMES does not match mmc.h's IC_* enum")


if __name__ == "__main__":
    check_enum()
    write_bmp(sys.argv[1], strip(16))
    write_bmp(sys.argv[2], strip(32))
    program_icon(sys.argv[3], ["COMPUTER"])
    extra = {4: ["EVENTS"], 5: ["SYSTEM"], 6: ["SCAN"], 7: ["DISK"]}
    for i, names in extra.items():
        if len(sys.argv) > i:
            program_icon(sys.argv[i], names)
