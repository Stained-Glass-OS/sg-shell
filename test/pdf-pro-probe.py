#!/usr/bin/python3
# SG PDF's professional-tools gate helper (test/pdf-pro-check.sh): makes the
# gate's documents and reads what SG PDF saved, from outside SG PDF --
# MuPDF (python3-pymupdf) for the structure, poppler's pdftotext and pdfsig
# for the text and the signatures.
#
#   make DIR              flat.pdf (a form drawn on paper: underscores, rules,
#                         small squares, a table), long.pdf (six pages, "Page
#                         N of the long document"), scan.png (a page of text
#                         as a picture), notes.txt, data.bin
#   fields FILE           "NAME|TYPE|VALUE|FORMAT-SCRIPT|CALC-SCRIPT" lines
#   text FILE [PAGE]      pdftotext's text (one page: PAGE, 1-based), one line
#   layout FILE           pdftotext -layout's text
#   drawings FILE PAGE    the number of paths on the page (0-based)
#   annots FILE           "PAGE TYPE" lines
#   attachments FILE      "NAME SIZE" lines
#   links FILE            "PAGE URI-or-TARGET" lines
#   pages FILE            the number of pages
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import os
import subprocess
import sys

import pymupdf as fitz


def make(d):
    os.makedirs(d, exist_ok=True)
    doc = fitz.open()
    p = doc.new_page(width=612, height=792)
    p.insert_text((72, 72), "Membership Application", fontsize=18, fontname="hebo")
    p.insert_text((72, 110), "Full name: ______________________________", fontsize=11)
    p.insert_text((72, 140), "Date of birth:", fontsize=11)
    p.draw_line((160, 142), (320, 142), width=0.7)
    p.insert_text((72, 170), "Email:", fontsize=11)
    p.draw_line((120, 172), (400, 172), width=0.7)
    p.draw_rect(fitz.Rect(72, 190, 82, 200), width=0.8)
    p.insert_text((88, 199), "Student", fontsize=10)
    p.draw_rect(fitz.Rect(172, 190, 182, 200), width=0.8)
    p.insert_text((188, 199), "Senior", fontsize=10)
    p.insert_text((72, 240), "Item", fontsize=10)
    p.insert_text((272, 240), "Qty", fontsize=10)
    p.insert_text((372, 240), "Price", fontsize=10)
    for row in range(2):
        y = 246 + row * 22
        for x0, x1 in ((72, 270), (270, 370), (370, 470)):
            p.draw_rect(fitz.Rect(x0, y, x1, y + 22), width=0.6)
    p.insert_text((72, 330), "Signature:", fontsize=11)
    p.draw_line((130, 332), (330, 332), width=0.7)
    doc.save(os.path.join(d, "flat.pdf"))
    doc = fitz.open()
    for i in range(6):
        pg = doc.new_page(width=612, height=792)
        pg.insert_text((72, 100), "Page %d of the long document" % (i + 1), fontsize=20)
        pg.draw_rect(fitz.Rect(72, 200, 540, 600), color=(0.2, 0.3, 0.8), fill=(0.85, 0.9, 1))
    doc.save(os.path.join(d, "long.pdf"))
    doc = fitz.open()
    sp = doc.new_page()
    sp.insert_text((72, 100), "The quick brown fox jumps over the lazy dog", fontsize=16)
    sp.insert_text((72, 140), "Invoice number 48213 due October 2026", fontsize=16)
    sp.get_pixmap(dpi=200).save(os.path.join(d, "scan.png"))
    with open(os.path.join(d, "notes.txt"), "w") as f:
        f.write("Plain text notes for the gate\nsecond line\n")
    with open(os.path.join(d, "data.bin"), "wb") as f:
        f.write(bytes(range(256)) * 20)


def main():
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "make":
        make(args[0])
    elif cmd == "fields":
        d = fitz.open(args[0])
        for p in d:
            for w in p.widgets():
                print("%s|%s|%s|%s|%s" % (w.field_name, w.field_type_string, w.field_value,
                                          " ".join((w.script_format or "").split()), " ".join((w.script_calc or "").split())))
    elif cmd == "text":
        extra = ["-f", args[1], "-l", args[1]] if len(args) > 1 else []
        out = subprocess.run(["pdftotext"] + extra + [args[0], "-"], capture_output=True, text=True).stdout
        print(" ".join(out.split()))
    elif cmd == "layout":
        print(subprocess.run(["pdftotext", "-layout", args[0], "-"], capture_output=True, text=True).stdout)
    elif cmd == "drawings":
        print(len(fitz.open(args[0])[int(args[1])].get_drawings()))
    elif cmd == "annots":
        d = fitz.open(args[0])
        for p in d:
            for a in p.annots():
                print("%d %s" % (p.number + 1, a.type[1]))
    elif cmd == "links":
        d = fitz.open(args[0])
        for p in d:
            for ln in p.get_links():
                print("%d %s" % (p.number + 1, ln.get("uri") or ln.get("page")))
    elif cmd == "attachments":
        d = fitz.open(args[0])
        for n in d.embfile_names():
            print("%s %d" % (n, len(d.embfile_get(n))))
    elif cmd == "pages":
        print(fitz.open(args[0]).page_count)
    else:
        sys.exit("unknown command")


if __name__ == "__main__":
    main()
