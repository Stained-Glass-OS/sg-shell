#!/usr/bin/python3
# The PDF Viewer gate's document, made at test time with cairo (nothing
# binary is committed): three US Letter pages with known words in known
# places, an outline (bookmarks) two levels deep, a link to page 3 and a
# web link, and a fourth page.
#
#   mkpdf.py OUT.pdf
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import sys

import cairo

W, H = 612, 792


def text(c, x, y, s, size=24, bold=False):
    c.select_font_face("DejaVu Sans", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD if bold else cairo.FONT_WEIGHT_NORMAL)
    c.set_font_size(size)
    c.move_to(x, y)
    c.show_text(s)


def main():
    s = cairo.PDFSurface(sys.argv[1], W, H)
    s.set_metadata(cairo.PDF_METADATA_TITLE, "Stained Glass PDF gate")
    c = cairo.Context(s)
    c.set_source_rgb(0, 0, 0)
    # page 1
    ch1 = s.add_outline(cairo.PDF_OUTLINE_ROOT, "Chapter One", "page=1 pos=[0 0]", cairo.PDF_OUTLINE_FLAG_OPEN)
    text(c, 72, 110, "Alpha document", 32, True)
    text(c, 72, 160, "The quick brown fox jumps over the lazy dog.", 18)
    c.tag_begin(cairo.TAG_LINK, "dest='third'")
    text(c, 72, 240, "Go to the third page")
    c.tag_end(cairo.TAG_LINK)
    c.tag_begin(cairo.TAG_LINK, "uri='https://example.com/'")
    text(c, 72, 300, "example.com")
    c.tag_end(cairo.TAG_LINK)
    s.add_outline(ch1, "Section One A", "page=1 pos=[0 200]", 0)
    c.show_page()
    # page 2: big words, a dark bar
    s.add_outline(cairo.PDF_OUTLINE_ROOT, "Chapter Two", "page=2 pos=[0 0]", 0)
    text(c, 72, 140, "Zebra crossing", 56, True)
    c.set_source_rgb(0.2, 0.1, 0.45)
    c.rectangle(72, 200, 468, 60)
    c.fill()
    c.set_source_rgb(0, 0, 0)
    text(c, 72, 330, "Second page words: xylophone quartz.")
    c.show_page()
    # page 3
    s.add_outline(cairo.PDF_OUTLINE_ROOT, "Chapter Three", "page=3 pos=[0 0]", 0)
    c.tag_begin(cairo.TAG_DEST, "name='third'")
    text(c, 72, 110, "Omega third page", 32, True)
    c.tag_end(cairo.TAG_DEST)
    text(c, 72, 170, "Another zebra appears here.")
    c.show_page()
    # page 4 (Letter too: cairo places every page's links by the last
    # page's height, so a page of another size would move them)
    text(c, 72, 110, "Delta fourth page", 28, True)
    c.show_page()
    s.finish()


if __name__ == "__main__":
    main()
