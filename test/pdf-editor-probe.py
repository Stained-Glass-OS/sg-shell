#!/usr/bin/python3
# SG PDF's editor gate helper (test/pdf-editor-check.sh): makes the gate's
# documents and reads the files SG PDF saved, from outside SG PDF -- MuPDF
# (python3-pymupdf) for the structure, poppler's pdftotext for the text, and
# qpdf's uncompressed dump for a raw search of every stream.
#
#   make DIR            edit.pdf (a paragraph, a picture, a box, a line; four
#                       pages with "Page N marker"), form.pdf (text field,
#                       check box, two radio buttons, a combo box), red.pdf
#                       (secrets in literal Tj strings -- a raw search finds
#                       them before redaction -- a picture with a red square,
#                       a form XObject, a hidden layer, metadata, an
#                       attachment, JavaScript), pic.png
#   text FILE [PW]      pdftotext's text, one line
#   words FILE PAGE     "x0 y0 x1 y1 word" lines (MuPDF)
#   images FILE PAGE    "x0 y0 x1 y1 xref" lines
#   drawings FILE PAGE  the number of paths
#   annots FILE         "PAGE TYPE CONTENTS" lines
#   fields FILE         "NAME=VALUE" lines
#   pages FILE          "N ROTATION FIRSTLINE" lines
#   raw FILE WORD...    the words found anywhere in the file (literal, hex,
#                       UTF-16), one a line
#   redpixels FILE      red pixels left in page 1's picture's middle
#   info FILE           "encrypted METHOD" (qpdf) and the metadata title
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import os
import struct
import subprocess
import sys
import zlib

import pymupdf as fitz


def png(path, w, h, pixel):
    rows = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def make(d):
    os.makedirs(d, exist_ok=True)
    sq = os.path.join(d, "square.png")
    png(sq, 100, 100, lambda x, y: (230, 20, 20) if 35 <= x < 65 and 35 <= y < 65 else (40, 120, 220))
    png(os.path.join(d, "pic.png"), 40, 30, lambda x, y: (20, 180, 60))
    # edit.pdf
    doc = fitz.open()
    p = doc.new_page(width=612, height=792)
    p.insert_textbox(fitz.Rect(72, 72, 400, 140), "The quick brown fox jumps over the lazy dog. A paragraph that wraps.",
                     fontname="helv", fontsize=12)
    p.insert_image(fitz.Rect(72, 300, 172, 400), filename=sq)
    p.draw_rect(fitz.Rect(300, 300, 400, 350), color=(0, 0, 0), fill=(0.9, 0.8, 0.1), width=1)
    p.draw_line((300, 420), (500, 420), color=(0.8, 0, 0), width=2)
    for i in range(2, 5):
        doc.new_page(width=612, height=792).insert_text((72, 100), "Page %d marker" % i, fontname="helv", fontsize=24)
    doc.save(os.path.join(d, "edit.pdf"), garbage=3, deflate=True)
    # form.pdf
    doc = fitz.open()
    p = doc.new_page(width=612, height=792)
    p.insert_text((72, 90), "Application form", fontname="helv", fontsize=16)
    for kind, name, rect, kw in (
            (fitz.PDF_WIDGET_TYPE_TEXT, "fullname", (150, 110, 400, 132), {"field_value": "", "text_fontsize": 11}),
            (fitz.PDF_WIDGET_TYPE_CHECKBOX, "agree", (150, 150, 168, 168), {"field_value": False}),
            (fitz.PDF_WIDGET_TYPE_RADIOBUTTON, "size", (150, 190, 168, 208), {"field_value": False}),
            (fitz.PDF_WIDGET_TYPE_RADIOBUTTON, "size", (200, 190, 218, 208), {"field_value": False}),
            (fitz.PDF_WIDGET_TYPE_COMBOBOX, "colour", (150, 230, 300, 252), {"choice_values": ["Red", "Green", "Blue"], "field_value": "Red"})):
        w = fitz.Widget()
        w.field_type = kind
        w.field_name = name
        w.rect = fitz.Rect(rect)
        for k, v in kw.items():
            setattr(w, k, v)
        p.add_widget(w)
    doc.save(os.path.join(d, "form.pdf"), garbage=3, deflate=True)
    # red.pdf
    doc = fitz.open()
    page = doc.new_page(width=612, height=792)
    page.insert_image(fitz.Rect(400, 500, 500, 600), filename=sq)
    src = fitz.open()
    src.new_page(width=200, height=40).insert_text((5, 25), "FORMSECRET", fontsize=14, fontname="helv")
    page.show_pdf_page(fitz.Rect(72, 640, 272, 680), src, 0)
    ocg = doc.add_ocg("Hidden notes", on=False)
    img = "/" + page.get_images(full=True)[0][7]
    form = "/" + [x[1] for x in page.get_xobjects() if x[2] == 0][0]
    content = ("0.85 0.85 0.85 rg 60 380 492 200 re f\n0 0 0 RG 1 w 60 480 m 552 480 l S 180 380 m 180 580 l S\n"
               "BT 0 g /F1 12 Tf\n1 0 0 1 72 720 Tm (Name: John Q Public) Tj\n1 0 0 1 72 700 Tm (SSN: 123-45-6789) Tj\n"
               "1 0 0 1 72 680 Tm (Phone: \\(555\\) 123-4567) Tj\n1 0 0 1 72 500 Tm (Keep this line TOPSECRET keep this too) Tj\n"
               "3 Tr 1 0 0 1 72 420 Tm (HIDDENTEXT invisible) Tj 0 Tr\nET\n"
               "/OC /L1 BDC BT 0 g /F1 12 Tf 1 0 0 1 72 400 Tm (LAYERSECRET on a layer that is off) Tj ET EMC\n"
               "q 100 0 0 100 400 192 cm %s Do Q\nq %s Do Q\n" % (img, form))
    font = doc.get_new_xref()
    doc.update_object(font, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
    t, res = doc.xref_get_key(page.xref, "Resources")
    res, pre = (int(res.split()[0]), "") if t == "xref" else (page.xref, "Resources/")
    doc.xref_set_key(res, pre + "Font", "<< /F1 %d 0 R >>" % font)
    doc.xref_set_key(res, pre + "Properties", "<< /L1 %d 0 R >>" % ocg)
    cont = page.get_contents()
    doc.update_stream(cont[0], content.encode("latin-1"), compress=False)
    if len(cont) > 1:
        doc.xref_set_key(page.xref, "Contents", "%d 0 R" % cont[0])
    doc.new_page(width=612, height=792).insert_text((72, 100), "TOPSECRET on page two", fontname="helv", fontsize=12)
    doc.set_metadata({"title": "SECRETMETA title"})
    doc.embfile_add("secret.txt", b"ATTACHSECRET", filename="secret.txt")
    js = doc.get_new_xref()
    doc.update_object(js, "<< /S /JavaScript /JS (app.alert\\('JSSECRET'\\)) >>")
    doc.xref_set_key(doc.pdf_catalog(), "OpenAction", "%d 0 R" % js)
    doc.save(os.path.join(d, "red.pdf"), garbage=3, deflate=False)


def main():
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "make":
        make(args[0])
    elif cmd == "text":
        out = subprocess.run(["pdftotext"] + (["-upw", args[1]] if len(args) > 1 else []) + [args[0], "-"],
                             capture_output=True, text=True).stdout
        print(" ".join(out.split()))
    elif cmd == "words":
        for w in fitz.open(args[0])[int(args[1])].get_text("words"):
            print("%d %d %d %d %s" % (w[0], w[1], w[2], w[3], w[4]))
    elif cmd == "images":
        for i in fitz.open(args[0])[int(args[1])].get_image_info(xrefs=True):
            print("%d %d %d %d %d" % (tuple(i["bbox"]) + (i["xref"],)))
    elif cmd == "drawings":
        print(len(fitz.open(args[0])[int(args[1])].get_drawings()))
    elif cmd == "annots":
        d = fitz.open(args[0])
        for p in d:
            for a in p.annots():
                print("%d %s %s" % (p.number + 1, a.type[1], " ".join((a.info.get("content") or "").split())))
    elif cmd == "fields":
        d = fitz.open(args[0])
        for p in d:
            for w in p.widgets():
                v = w.field_value
                print("%s=%s" % (w.field_name, v))
    elif cmd == "pages":
        d = fitz.open(args[0])
        for p in d:
            t = " ".join(p.get_text().split())
            print("%d %d %s" % (p.number + 1, p.rotation, t[:40]))
    elif cmd == "raw":
        tmp = args[0] + ".qdf"
        subprocess.run(["qpdf", "--qdf", "--object-streams=disable", "--decode-level=all", args[0], tmp], capture_output=True)
        data = open(tmp, "rb").read()
        os.unlink(tmp)
        for w in args[1:]:
            b = w.encode("latin-1")
            if any(f in data for f in (b, b.hex().encode(), b.hex().upper().encode(), w.encode("utf-16-be"),
                                       w.encode("utf-16-be").hex().encode(), w.encode("utf-16-be").hex().upper().encode())):
                print(w)
    elif cmd == "redpixels":
        d = fitz.open(args[0])
        info = d[0].get_image_info(xrefs=True)
        if not info:
            print(-1)
            return
        pix = fitz.Pixmap(d, info[0]["xref"])
        if pix.n > 3:
            pix = fitz.Pixmap(fitz.csRGB, pix)
        print(sum(1 for y in range(35, 65) for x in range(35, 65) if pix.pixel(x, y)[0] > 200 and pix.pixel(x, y)[1] < 60))
    elif cmd == "info":
        enc = subprocess.run(["qpdf", "--show-encryption"] + (["--password=" + args[1]] if len(args) > 1 else []) + [args[0]],
                             capture_output=True, text=True).stdout
        method = "none" if "not encrypted" in enc else ("AESv3" if "AESv3" in enc else "other")
        print("encrypted " + method)
        try:
            d = fitz.open(args[0])
            if d.needs_pass and len(args) > 1:
                d.authenticate(args[1])
            print("title " + ((d.metadata or {}).get("title") or ""))
            print("embfiles %d" % d.embfile_count())
        except Exception as e:  # noqa: BLE001
            print("error " + str(e))


if __name__ == "__main__":
    main()
