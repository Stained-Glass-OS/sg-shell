#!/usr/bin/python3
"""SG Office -- the templates and defaults as a LibreOffice reads them: a new
document, spreadsheet and presentation made through the payload's .xcd must
come out as Office's do. Runs a headless LibreOffice on a profile of its own.

    office-templates.py SRCDIR PAYLOAD

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import os
import sys
import tempfile

src, payload = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(src, "office/parity"))
from lo import Office  # noqa: E402

fails = 0


def check(what, ok, got=""):
    global fails
    print("  %s  %s%s" % ("ok  " if ok else "BAD ", what, "" if ok else "  (got %s)" % (got,)))
    fails += not ok


tmp = tempfile.mkdtemp(dir=os.environ.get("TMPDIR", "/var/tmp"))
reg = os.path.join(tmp, "reg")
os.makedirs(reg)
xcd = open(os.path.join(payload, "sg-office.xcd.in")).read().replace("@TEMPLATEDIR@", os.path.join(payload, "templates"))
open(os.path.join(reg, "sg-office.xcd"), "w").write(xcd)
o = Office(xcd=reg, env={"LANG": "en_US.UTF-8", "LC_ALL": "en_US.UTF-8"})
try:
    check("the product is SG Office", o.config("/org.openoffice.Setup/Product").ooName == "SG Office")
    d = o.new("swriter")
    st = d.StyleFamilies.getByName("ParagraphStyles").getByName("Standard")
    check("body text Calibri 11 pt", (st.CharFontName, st.CharHeight) == ("Calibri", 11.0), (st.CharFontName, st.CharHeight))
    check("line spacing 1.08", (st.ParaLineSpacing.Mode, st.ParaLineSpacing.Height) == (0, 108), st.ParaLineSpacing.Height)
    check("8 pt after a paragraph", abs(st.ParaBottomMargin - 282) <= 1, st.ParaBottomMargin)
    h = d.StyleFamilies.getByName("ParagraphStyles").getByName("Heading 1")
    check("Heading 1 Calibri Light 16 pt", (h.CharFontName, h.CharHeight) == ("Calibri Light", 16.0), (h.CharFontName, h.CharHeight))
    ps = d.StyleFamilies.getByName("PageStyles").getByName("Standard")
    check("US Letter with 1-inch margins in en-US", (ps.Width, ps.Height, ps.LeftMargin, ps.TopMargin) == (21590, 27940, 2540, 2540),
          (ps.Width, ps.Height, ps.LeftMargin))
    d.close(True)
    d = o.new("scalc")
    cs = d.StyleFamilies.getByName("CellStyles").getByName("Default")
    sh = d.Sheets.getByIndex(0)
    check("cells Calibri 11 pt", (cs.CharFontName, cs.CharHeight) == ("Calibri", 11.0), (cs.CharFontName, cs.CharHeight))
    check("columns 8.43 characters (2.258 cm), rows 15 pt", (sh.Columns.getByIndex(0).Width, sh.Rows.getByIndex(0).Height) == (2258, 529),
          (sh.Columns.getByIndex(0).Width, sh.Rows.getByIndex(0).Height))
    check("one sheet, Sheet1", (d.Sheets.Count, sh.Name) == (1, "Sheet1"), (d.Sheets.Count, sh.Name))
    d.close(True)
    d = o.new("simpress")
    p = d.DrawPages.getByIndex(0)
    check("slides 16:9", (p.Width, p.Height) == (33867, 19050), (p.Width, p.Height))
    m = d.MasterPages.getByIndex(0).Name
    t = d.StyleFamilies.getByName(m).getByName("title")
    b = d.StyleFamilies.getByName(m).getByName("outline1")
    check("titles Calibri Light 44 pt, text Calibri 28 pt",
          (t.CharFontName, t.CharHeight, b.CharFontName, b.CharHeight) == ("Calibri Light", 44.0, "Calibri", 28.0),
          (t.CharFontName, t.CharHeight, b.CharFontName, b.CharHeight))
    kinds = [p.getByIndex(i).ShapeType.rsplit(".", 1)[1] for i in range(p.Count)]
    check("the first slide is a title slide (title and subtitle to click into)",
          kinds == ["TitleTextShape", "SubtitleShape"], kinds)
    d.close(True)
finally:
    o.close()
sys.exit(1 if fails else 0)
