#!/usr/bin/python3
"""SG Office Functions -- the logic's unit gate, without LibreOffice.

Each case is an Excel documentation example or a known Excel behaviour; the
values go in as LibreOffice hands them to an add-in (numbers as floats,
ranges as tuples of rows, an empty cell as "", a left-out argument as None).

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sgoffice_functions as f  # noqa: E402

FAILS = 0


def err(v):
    return f.is_error(v) and {32767: "#N/A", 519: "#VALUE!", 503: "#NUM!", 532: "#DIV/0!"}.get(f.error_code(v), "?")


def show(v):
    if isinstance(v, tuple):
        return tuple(show(x) for x in v)
    return err(v) or v


def check(name, got, want):
    global FAILS
    g = show(got)
    ok = g == want or (isinstance(want, float) and isinstance(g, float) and math.isclose(g, want, rel_tol=1e-12))
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok else "   got %r, want %r" % (g, want)))
    FAILS += not ok


T = "Little Red Riding Hood's red hood"
check("TEXTBEFORE first", f.textbefore(T, "Red"), "Little ")
check("TEXTBEFORE 2nd, any case", f.textbefore(T, "red", 2.0, 1.0), "Little Red Riding Hood's ")
check("TEXTBEFORE from the end", f.textbefore("a-b-c", "-", -1.0), "a-b")
check("TEXTBEFORE match_end", f.textbefore("abc", "x", None, None, 1.0), "abc")
check("TEXTBEFORE not found", f.textbefore("abc", "x"), "#N/A")
check("TEXTBEFORE if_not_found", f.textbefore("abc", "x", None, None, None, "none"), "none")
check("TEXTBEFORE instance 0", f.textbefore("abc", "b", 0.0), "#VALUE!")
check("TEXTBEFORE several delimiters", f.textbefore("a, b; c", ((",", ";"),), 2.0), "a, b")
check("TEXTBEFORE empty delimiter", f.textbefore("abc", ""), "")
check("TEXTAFTER", f.textafter(T, "Red"), " Riding Hood's red hood")
check("TEXTAFTER last", f.textafter(T, "red", -1.0), " hood")
check("TEXTAFTER empty delimiter", f.textafter("abc", ""), "abc")
check("TEXTSPLIT columns", f.textsplit("Dakota Lennon Sanchez", " ", array=True), (("Dakota", "Lennon", "Sanchez"),))
check("TEXTSPLIT rows and columns", f.textsplit("a,b;c,d", ",", ";", array=True), (("a", "b"), ("c", "d")))
check("TEXTSPLIT pads #N/A", f.textsplit("a,b;c", ",", ";", array=True), (("a", "b"), ("c", "#N/A")))
check("TEXTSPLIT pad_with", f.textsplit("a,b;c", ",", ";", None, None, "-", array=True), (("a", "b"), ("c", "-")))
check("TEXTSPLIT ignore_empty", f.textsplit("a;;b", None, ";", 1.0, array=True), (("a",), ("b",)))
check("TEXTSPLIT case-insensitive", f.textsplit("aXbxc", "x", None, None, 1.0, array=True), (("a", "b", "c"),))
check("TEXTSPLIT no delimiter", f.textsplit("abc", None, None, array=True), (("#VALUE!",),))
check("VSTACK", f.vstack(((1.0, 2.0),), (((3.0, 4.0),),), array=True), ((1.0, 2.0), (3.0, 4.0)))
check("VSTACK pads #N/A", f.vstack(((1.0, 2.0),), (3.0,), array=True), ((1.0, 2.0), (3.0, "#N/A")))
check("HSTACK", f.hstack(((1.0,), (2.0,)), (3.0,), array=True), ((1.0, 3.0), (2.0, "#N/A")))
check("TOCOL", f.tocol(((1.0, 2.0), (3.0, 4.0)), array=True), ((1.0,), (2.0,), (3.0,), (4.0,)))
check("TOCOL by column", f.tocol(((1.0, 2.0), (3.0, 4.0)), None, 1.0, array=True), ((1.0,), (3.0,), (2.0,), (4.0,)))
check("TOCOL ignore blanks", f.tocol(((1.0, ""), ("", 4.0)), 1.0, array=True), ((1.0,), (4.0,)))
check("TOROW", f.torow(((1.0, 2.0), (3.0, 4.0)), array=True), ((1.0, 2.0, 3.0, 4.0),))
check("WRAPROWS", f.wraprows(((1.0, 2.0, 3.0, 4.0, 5.0),), 2.0, array=True), ((1.0, 2.0), (3.0, 4.0), (5.0, "#N/A")))
check("WRAPCOLS", f.wrapcols(((1.0, 2.0, 3.0, 4.0, 5.0),), 2.0, 0.0, array=True), ((1.0, 3.0, 5.0), (2.0, 4.0, 0.0)))
check("WRAPROWS a 2-D array", f.wraprows(((1.0, 2.0), (3.0, 4.0)), 2.0, array=True), (("#VALUE!",),))
check("TAKE rows", f.take(((1.0, 2.0), (3.0, 4.0), (5.0, 6.0)), 2.0, array=True), ((1.0, 2.0), (3.0, 4.0)))
check("TAKE last", f.take(((1.0, 2.0), (3.0, 4.0), (5.0, 6.0)), -1.0, -1.0, array=True), ((6.0,),))
check("TAKE columns only", f.take(((1.0, 2.0, 3.0),), None, 2.0, array=True), ((1.0, 2.0),))
check("DROP", f.drop(((1.0, 2.0, 3.0), (4.0, 5.0, 6.0)), 1.0, 1.0, array=True), ((5.0, 6.0),))
check("DROP everything", f.drop(((1.0,),), 1.0, array=True), (("#VALUE!",),))
check("CHOOSEROWS", f.chooserows(((1.0, 2.0), (3.0, 4.0), (5.0, 6.0)), -1.0, (1.0,), array=True), ((5.0, 6.0), (1.0, 2.0)))
check("CHOOSECOLS", f.choosecols(((1.0, 2.0, 3.0),), 3.0, (1.0,), array=True), ((3.0, 1.0),))
check("CHOOSECOLS out of range", f.choosecols(((1.0, 2.0),), 3.0, array=True), (("#VALUE!",),))
check("EXPAND", f.expand(((1.0, 2.0),), 2.0, 3.0, 0.0, array=True), ((1.0, 2.0, 0.0), (0.0, 0.0, 0.0)))
check("EXPAND smaller", f.expand(((1.0, 2.0),), 1.0, 1.0, array=True), (("#VALUE!",),))
check("TRIMRANGE", f.trimrange((("", ""), ("", 1.0), ("", 2.0), ("", "")), array=True), ((1.0,), (2.0,)))
check("ARRAYTOTEXT concise", f.arraytotext(((1.0, "a"), (2.5, 2.0))), "1, a, 2.5, 2")
check("ARRAYTOTEXT strict", f.arraytotext(((1.0, "a"), (2.5, 2.0)), 1.0), '{1,"a";2.5,2}')
check("VALUETOTEXT strict", f.valuetotext("abc", 1.0), '"abc"')
check("VALUETOTEXT number", f.valuetotext(12.5), "12.5")
check("REGEXTEST", f.regextest("alpha@example.com", "^[^@]+@[^@]+$"), 1.0)
check("REGEXTEST case", f.regextest("ABC", "abc"), 0.0)
check("REGEXTEST case-insensitive", f.regextest("ABC", "abc", 1.0), 1.0)
check("REGEXEXTRACT", f.regexextract("Phone: 555-1234", "[0-9]{3}-[0-9]{4}", array=True), (("555-1234",),))
check("REGEXEXTRACT all", f.regexextract("a1b22c333", "[0-9]+", 1.0, array=True), (("1",), ("22",), ("333",)))
check("REGEXEXTRACT groups", f.regexextract("John Smith", "(\\w+) (\\w+)", 2.0, array=True), (("John", "Smith"),))
check("REGEXEXTRACT none", f.regexextract("abc", "[0-9]", array=True), (("#N/A",),))
check("REGEXREPLACE", f.regexreplace("a1b22c333", "[0-9]+", "#"), "a#b#c#")
check("REGEXREPLACE groups", f.regexreplace("Smith, John", "(\\w+), (\\w+)", "$2 $1"), "John Smith")
check("REGEXREPLACE 2nd", f.regexreplace("a1b2c3", "[0-9]", "#", 2.0), "a1b#c3")
check("DBCS", f.dbcs("ABC 1"), "ＡＢＣ　１")
check("DBCS kana", f.dbcs("ｶﾞ"), "ガ")
check("PERCENTOF", f.percentof(((10.0, 30.0),), ((10.0, 30.0, 60.0),)), 0.4)
check("PERCENTOF by zero", f.percentof(1.0, 0.0), "#DIV/0!")
check("BINOM.DIST.RANGE", f.binomdistrange(60.0, 0.75, 48.0), 0.08397496742904752)
check("BINOM.DIST.RANGE s>n", f.binomdistrange(6.0, 0.5, 7.0), "#NUM!")
check("IMSINH", f.imsinh("4+3i"), "-27.0168132580039+3.85373803791938i")
check("IMCOSH j", f.imcosh("1+j"), "0.833730025131149+0.988897705762865j")
check("IMTAN real", f.imtan(0.0), "0")
check("IMSEC bad", f.imsec("abc"), "#NUM!")
check("general format", (f.general(1e15), f.general(0.1 + 0.2), f.general(1e-10), f.general(-2.5)),
      ("1E+15", "0.3", "1E-10", "-2.5"))
print("RESULT: %s (%d failed)" % ("PASS" if not FAILS else "FAIL", FAILS))
sys.exit(1 if FAILS else 0)
