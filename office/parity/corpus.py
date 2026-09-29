#!/usr/bin/python3
"""SG Office -- the Excel formula-compatibility corpus and its runner.

    corpus.py [--xlsx OUT.xlsx] [--run] [--json RES.json] [--report OUT.md]
              [--baseline BASE.json] [--profile DIR] [--oxt FILE] [--xcd DIR]

Reads corpus-*.txt (one case a line, see corpus-1.txt), builds one .xlsx
the way Excel writes one -- "_xlfn." prefixes on post-2007 functions, LET and
LAMBDA parameters as "_xlpm.", dynamic-array formulas as one-cell array
formulas with Excel's cell metadata -- and, with --run, opens it in a headless
LibreOffice, recalculates everything, reads every result, saves the workbook
back to .xlsx and checks that each function's name survives the round trip
as Excel spells it. Each case ends up:

    match     the result is Excel's
    differs   the function is known, the result is not Excel's
    missing   the function is unknown (#NAME?) or its syntax does not parse
    export    right result, but saving to .xlsx writes a name Excel would not read

--baseline makes it a gate: every case the baseline records as a match must
still match (a regression fails), and the report shows what got better.

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import argparse
import cmath  # noqa: F401  (py: expressions)
import datetime
import glob
import json
import math
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from xlsxw import Workbook, ref  # noqa: E402

# Functions Excel writes with a "_xlfn." prefix (newer than Excel 2007): the
# file format's "future functions", as XlsxWriter (tested against Excel) lists
# them -- NETWORKDAYS.INTL, WORKDAY.INTL and ISO.CEILING have none -- and
# FILTER and SORT also "_xlws.". BAHTTEXT is here as LibreOffice reads and
# writes it; which spelling Excel uses is unverified.
FUTURE = set("""
ACOT ACOTH AGGREGATE ARABIC BASE BETA.DIST BETA.INV BINOM.DIST BINOM.DIST.RANGE BINOM.INV BITAND BITLSHIFT
BITOR BITRSHIFT BITXOR CEILING.MATH CEILING.PRECISE CHISQ.DIST CHISQ.DIST.RT CHISQ.INV CHISQ.INV.RT CHISQ.TEST
COMBINA CONCAT CONFIDENCE.NORM CONFIDENCE.T COT COTH COVARIANCE.P COVARIANCE.S CSC CSCH DAYS DECIMAL ENCODEURL
ERF.PRECISE ERFC.PRECISE EXPON.DIST F.DIST F.DIST.RT F.INV F.INV.RT F.TEST FILTERXML FLOOR.MATH FLOOR.PRECISE
FORECAST.ETS FORECAST.ETS.CONFINT FORECAST.ETS.SEASONALITY FORECAST.ETS.STAT FORECAST.LINEAR FORMULATEXT GAMMA
GAMMA.DIST GAMMA.INV GAMMALN.PRECISE GAUSS HYPGEOM.DIST IFNA IFS IMCOSH IMCOT IMCSC IMCSCH IMSEC IMSECH IMSINH
IMTAN ISFORMULA ISOWEEKNUM LOGNORM.DIST LOGNORM.INV MAXIFS MINIFS MODE.MULT MODE.SNGL MUNIT
NEGBINOM.DIST NORM.DIST NORM.INV NORM.S.DIST NORM.S.INV NUMBERVALUE PDURATION PERCENTILE.EXC
PERCENTILE.INC PERCENTRANK.EXC PERCENTRANK.INC PERMUTATIONA PHI POISSON.DIST QUARTILE.EXC QUARTILE.INC RANK.AVG
RANK.EQ RRI SEC SECH SHEET SHEETS SKEW.P STDEV.P STDEV.S SWITCH T.DIST T.DIST.2T T.DIST.RT T.INV T.INV.2T T.TEST
TEXTJOIN UNICHAR UNICODE VAR.P VAR.S WEBSERVICE WEIBULL.DIST XOR Z.TEST BAHTTEXT
XLOOKUP XMATCH LET LAMBDA MAP REDUCE SCAN BYROW BYCOL MAKEARRAY ISOMITTED SEQUENCE RANDARRAY UNIQUE SORTBY
TEXTBEFORE TEXTAFTER TEXTSPLIT VSTACK HSTACK TOROW TOCOL WRAPROWS WRAPCOLS TAKE DROP CHOOSEROWS CHOOSECOLS EXPAND
ARRAYTOTEXT VALUETOTEXT IMAGE GROUPBY PIVOTBY PERCENTOF REGEXTEST REGEXEXTRACT REGEXREPLACE TRIMRANGE
STOCKHISTORY FIELDVALUE TRANSLATE DETECTLANGUAGE ANCHORARRAY SINGLE
""".split())
XLWS = {"FILTER", "SORT"}

ERRORS = {"#NULL!": 521, "#DIV/0!": 532, "#VALUE!": 519, "#REF!": 524, "#NAME?": 525, "#NUM!": 503, "#N/A": 32767}
LO_ERR = {v: k for k, v in ERRORS.items()}
LO_ERR.update({502: "#NUM!", 504: "#VALUE!", 511: "#VALUE!", 523: "#NUM!"})   # displayed as Err:5xx


def serial(d):
    return (d - datetime.date(1899, 12, 30)).days


# ---- helpers for py: expected values -------------------------------------------------------------
def _helpers():
    import numpy as np
    import scipy.special as sp
    import scipy.stats as st

    def chisq_test(act, exp):
        a, e = np.array(act, float), np.array(exp, float)
        stat = ((a - e) ** 2 / e).sum()
        r, c = a.shape
        df = (r - 1) * (c - 1) if r > 1 and c > 1 else (r * c - 1)
        return st.chi2.sf(stat, df)

    def f_test(a, b):
        va, vb = np.var(a, ddof=1), np.var(b, ddof=1)
        f = va / vb
        p = st.f.cdf(f, len(a) - 1, len(b) - 1)
        return 2 * min(p, 1 - p)

    def growth(ys, xs, x):
        m, b = np.polyfit(xs, np.log(ys), 1)
        return math.exp(b + m * x)

    def linest_r2(ys, xs):
        return np.corrcoef(xs, ys)[0][1] ** 2

    def steyx(ys, xs):
        m, b = np.polyfit(xs, ys, 1)
        res = np.array(ys) - (m * np.array(xs) + b)
        return math.sqrt((res ** 2).sum() / (len(ys) - 2))

    def fv(r, n, p, v=0, t=0):
        if r == 0:
            return -(v + p * n)
        return -(v * (1 + r) ** n + p * (1 + r * t) * ((1 + r) ** n - 1) / r)

    def pv(r, n, p, f=0, t=0):
        return -(f + p * (1 + r * t) * ((1 + r) ** n - 1) / r) / (1 + r) ** n

    def pmt(r, n, v, f=0, t=0):
        return -(v * (1 + r) ** n + f) * r / ((1 + r * t) * ((1 + r) ** n - 1))

    def ipmt(r, per, n, v, f=0, t=0):
        p = pmt(r, n, v, f, t)
        if per == 1:
            return 0 if t == 1 else -v * r
        # interest on the balance after per-1 payments
        bal = -fv(r, per - 1, p, v, t)
        return -bal * r if t == 0 else -bal * r / (1 + r)

    def nper(r, p, v, f=0, t=0):
        return math.log((p * (1 + r * t) - f * r) / (p * (1 + r * t) + v * r)) / math.log(1 + r)

    def rate(n, p, v, f=0, t=0):
        from scipy.optimize import brentq
        return brentq(lambda r: fv(r, n, p, v, t) - f if r else -(v + p * n) - f, 1e-9, 1)

    def npv0(r, vals):
        return sum(v / (1 + r) ** i for i, v in enumerate(vals))

    def irr(vals):
        from scipy.optimize import brentq
        return brentq(lambda r: npv0(r, vals), -0.99, 1)

    def mirr(vals, fr, rr):
        n = len(vals)
        pos = sum(v * (1 + rr) ** (n - 1 - i) for i, v in enumerate(vals) if v > 0)
        neg = sum(v / (1 + fr) ** i for i, v in enumerate(vals) if v < 0)
        return (-pos / neg) ** (1 / (n - 1)) - 1

    def xnpv(r, vals, dates):
        return sum(v / (1 + r) ** ((d - dates[0]) / 365) for v, d in zip(vals, dates))

    def xirr(vals, dates):
        from scipy.optimize import brentq
        return brentq(lambda r: xnpv(r, vals, dates), -0.9, 5)

    return dict(math=math, cmath=cmath, np=np, sp=sp, st=st, chisq_test=chisq_test, f_test=f_test,
                growth=growth, linest_r2=linest_r2, steyx=steyx, fv=fv, pv=pv, pmt=pmt, ipmt=ipmt, nper=nper,
                rate=rate, irr=irr, mirr=mirr, xnpv=xnpv, xirr=xirr)


# ---- parsing the corpus -------------------------------------------------------------------------------
def split_strings(text):
    """[(is_string, piece)] -- Excel string literals ("..." with "") kept whole."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text[i] == '"':
            j = i + 1
            while j < n:
                if text[j] == '"':
                    if j + 1 < n and text[j + 1] == '"':
                        j += 2
                        continue
                    break
                j += 1
            out.append((True, text[i:j + 1]))
            i = j + 1
        else:
            j = text.find('"', i)
            j = n if j < 0 else j
            out.append((False, text[i:j]))
            i = j
    return out


FUNC_RE = re.compile(r'(?<![A-Za-z0-9_.!\\])([A-Z][A-Z0-9]*(?:\.[A-Z0-9]+)*)\(')


def excel_file_formula(f):
    """The formula as Excel stores it: prefixes on the newer functions."""
    def fix(m):
        name = m.group(1)
        if name in XLWS:
            return "_xlfn._xlws." + name + "("
        if name in FUTURE:
            return "_xlfn." + name + "("
        return m.group(0)
    return "".join(p if s else FUNC_RE.sub(fix, p) for s, p in split_strings(f))


def parse_literal(s, env):
    s = s.strip()
    if s.startswith('"') and s.endswith('"'):
        return s[1:-1].replace('""', '"')
    if s in ("TRUE", "FALSE"):
        return s == "TRUE"
    if s in ERRORS:
        return ("error", s)
    if s in ("any", "anyerr", "anynum"):
        return (s,)
    if s.startswith("date:"):
        y, m, d = map(int, s[5:].split("-"))
        return float(serial(datetime.date(y, m, d)))
    if s.startswith("py:"):
        return float(eval(s[3:], env))
    if s.startswith("{"):
        rows = []
        for row in s[1:-1].split(";"):
            rows.append([parse_literal(c, env) for c in split_top(row, ",")])
        return ("array", rows)
    return float(s)


def split_top(text, sep):
    parts, cur = [], ""
    for is_s, p in split_strings(text):
        if is_s:
            cur += p
            continue
        bits = p.split(sep)
        cur += bits[0]
        for b in bits[1:]:
            parts.append(cur)
            cur = b
    parts.append(cur)
    return parts


class Case:
    pass


def load_corpus(files):
    env = _helpers()
    cases, cat = [], ""
    for path in files:
        for no, line in enumerate(open(path, encoding="utf-8"), 1):
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            if line.startswith("@"):
                cat = line[1:].strip()
                continue
            name, rest = line.split(" :: ", 1)
            formula, exp = rest.rsplit(" => ", 1)
            c = Case()
            c.name, c.category, c.src = name.strip(), cat, "%s:%d" % (os.path.basename(path), no)
            c.formula = formula.strip()
            c.mode, c.spill = "plain", None
            m = re.search(r"\s*\[(da|cse|spill (\d+)x(\d+))\]\s*$", exp)
            if m:
                exp = exp[:m.start()]
                c.mode = m.group(1).split()[0]
                if m.group(2):
                    c.spill = (int(m.group(2)), int(m.group(3)))
            c.tol = None
            if " ~ " in exp:
                exp, tol = exp.rsplit(" ~ ", 1)
                c.tol = tol.strip()
            c.expected = parse_literal(exp, env)
            c.file_formula = excel_file_formula(c.formula)
            cases.append(c)
    for i, c in enumerate(cases):
        c.id = "%s#%d" % (c.name, sum(1 for d in cases[:i] if d.name == c.name) + 1)
    return cases


# ---- the workbook ----------------------------------------------------------------------------------------
DATA = [["Name", "Region", "Units", "Price", "Date"],
        ["Ann", "East", 10, 2.5, datetime.date(2024, 1, 15)],
        ["Bob", "West", 20, 3, datetime.date(2024, 2, 20)],
        ["Cid", "East", 15, 4, datetime.date(2024, 3, 10)],
        ["Dee", "North", 30, 2, datetime.date(2024, 1, 5)],
        ["Eve", "West", 25, 5, datetime.date(2024, 4, 1)],
        ["Fay", "East", 5, 10, datetime.date(2024, 2, 29)]]


def build(cases, path):
    wb = Workbook()
    t = wb.sheet("Tests")
    d = wb.sheet("Data")
    for r, row in enumerate(DATA):
        for c, v in enumerate(row):
            d.value(r, c, float(serial(v)) if isinstance(v, datetime.date) else v)
    d.formula(1, 5, "C2*D2")                                   # F2
    for r, v in enumerate(["Region", "East"]):
        d.value(r, 6, v)                                       # G1:G2
    d.value(0, 7, "Region"); d.value(0, 8, "Units")            # H1:I2
    d.value(1, 7, "West"); d.value(1, 8, ">20")
    for r in range(10):
        d.value(r, 10, r + 1)                                  # K1:K10
    d.formula(1, 11, '""')                                     # L2 (L1, L3 empty)
    d.value(3, 11, True); d.value(4, 11, False); d.value(5, 11, 0.5)   # L4:L6
    for r, v in enumerate(["apple", "Banana", "cherry", "apple", "date"]):
        d.value(r, 12, v)                                      # M1:M5
    d.formula(0, 13, "_xlfn.SEQUENCE(3)", array_ref="N1:N3", dynamic=True)
    wb.defined_names.append(("SalesUnits", "Data!$C$2:$C$7"))
    t.value(0, 0, "Case"); t.value(0, 1, "Result")
    row, spill_row = 1, len(cases) + 5
    for c in cases:
        t.value(row, 0, c.id)
        if c.spill:
            nr, nc = c.spill
            c.cell = (spill_row, 3)
            t.value(spill_row, 2, c.id)
            t.formula(spill_row, 3, c.file_formula,
                      array_ref="%s:%s" % (ref(spill_row, 3), ref(spill_row + nr - 1, 3 + nc - 1)), dynamic=True)
            spill_row += nr + 1
        else:
            c.cell = (row, 1)
            if c.mode == "da":
                t.formula(row, 1, c.file_formula, array_ref=ref(row, 1), dynamic=True)
            elif c.mode == "cse":
                t.formula(row, 1, c.file_formula, array_ref=ref(row, 1))
            else:
                t.formula(row, 1, c.file_formula)
        row += 1
    wb.save(path)


# ---- running it ------------------------------------------------------------------------------------------
def read_cell(sheet, r, c):
    cell = sheet.getCellByPosition(c, r)
    err = cell.getError()
    kind = cell.FormulaResultType2 if cell.getType().value == "FORMULA" else None
    if err:
        return ("error", err, cell.getString())
    if kind == 2 or cell.getType().value == "TEXT":
        return ("text", cell.getString())
    s = cell.getString()
    if s in ("TRUE", "FALSE"):
        return ("bool", s == "TRUE", cell.getValue())
    if cell.getType().value == "EMPTY":
        return ("empty",)
    return ("num", cell.getValue())


def close_enough(got, exp, tol):
    if tol == "0":
        return got == exp
    if tol:
        if tol.endswith("r"):
            return abs(got - exp) <= float(tol[:-1]) * max(abs(exp), 1e-300)
        return abs(got - exp) <= float(tol)
    return abs(got - exp) <= 1e-9 * max(abs(exp), 1.0)


def judge_value(got, exp, tol):
    """(ok, shown) for one value against one expected value."""
    shown = describe(got)
    if isinstance(exp, tuple) and exp[0] == "error":
        return got[0] == "error" and LO_ERR.get(got[1]) == exp[1] and got[2] == exp[1], shown
    if got[0] == "error":
        return False, shown
    if isinstance(exp, bool):
        return got[0] == "bool" and got[1] == exp, shown
    if isinstance(exp, str):
        return got[0] == "text" and got[1] == exp, shown
    if isinstance(exp, float) or isinstance(exp, int):
        if got[0] == "empty":
            got = ("num", 0.0)
        return got[0] == "num" and close_enough(got[1], float(exp), tol), shown
    return False, shown


def describe(got):
    if got[0] == "error":
        return got[2] if got[2] else "Err:%d" % got[1]
    if got[0] == "text":
        return '"%s"' % got[1]
    if got[0] == "bool":
        return "TRUE" if got[1] else "FALSE"
    if got[0] == "empty":
        return "(empty)"
    return "%.15g" % got[1]


def general_num(x):
    return "%.15g" % x


def describe_expected(exp):
    if isinstance(exp, tuple):
        if exp[0] == "error":
            return exp[1]
        if exp[0] == "array":
            return "{" + ";".join(",".join(describe_expected(v) for v in row) for row in exp[1]) + "}"
        return {"any": "(any value)", "anyerr": "(recognised; any result)", "anynum": "(any number)"}[exp[0]]
    if isinstance(exp, bool):
        return "TRUE" if exp else "FALSE"
    if isinstance(exp, str):
        return '"%s"' % exp
    return "%.15g" % exp


def unresolved(formula):
    return bool(re.search(r"_xlfn\.|_xlpm\.|_xleta\.|_xlws\.", formula, re.I))


def run(cases, xlsx, out_xlsx, profile=None, oxt=None, xcd=None, bundled=None, wine=None, seed=None):
    from lo import Office
    import uno  # noqa: F401
    env = {"LANG": "en_US.UTF-8", "LC_ALL": "en_US.UTF-8"}
    office = Office(profile=profile, xcd=xcd, oxt=oxt, bundled=bundled, env=env, wine=wine, seed=seed)
    try:
        doc = office.load(xlsx)
        doc.calculateAll()
        sheet = doc.Sheets.getByName("Tests")
        for c in cases:
            r, col = c.cell
            cell = sheet.getCellByPosition(col, r)
            c.lo_formula = cell.getFormula()
            if c.spill:
                nr, nc = c.spill
                c.got = [[read_cell(sheet, r + i, col + j) for j in range(nc)] for i in range(nr)]
            else:
                c.got = read_cell(sheet, r, col)
        office.store(doc, out_xlsx, "Calc MS Excel 2007 XML")
        doc.close(True)
    finally:
        office.close()
    exported = read_exported(out_xlsx)
    for c in cases:
        c.exported, c.saved_error = exported.get(ref(*c.cell), (None, None))


def read_exported(path):
    """{A1: formula text} from the saved workbook's Tests sheet."""
    z = zipfile.ZipFile(path)
    wb = z.read("xl/workbook.xml").decode()
    rels = z.read("xl/_rels/workbook.xml.rels").decode()
    m = re.search(r'<sheet [^>]*name="Tests"[^>]*r:id="([^"]+)"', wb)
    target = re.search(r'Id="%s"[^>]*Target="([^"]+)"' % m.group(1), rels) or \
        re.search(r'Target="([^"]+)"[^>]*Id="%s"' % m.group(1), rels)
    xml = z.read("xl/" + target.group(1).lstrip("/").replace("xl/", "")).decode()
    out = {}
    from html import unescape
    for cm in re.finditer(r'<c r="([A-Z]+[0-9]+)"([^>]*)>(.*?)</c>', xml, re.S):
        fm = re.search(r"<f[^>]*>(.*?)</f>", cm.group(3), re.S)
        vm = re.search(r"<v[^>]*>(.*?)</v>", cm.group(3), re.S)
        err = unescape(vm.group(1)) if vm and 't="e"' in cm.group(2) else None
        if fm:
            out[cm.group(1)] = (unescape(fm.group(1)), err)
    return out


def judge(c):
    exp = c.expected
    if unresolved(c.lo_formula):
        c.status, c.note = "missing", "not known (#NAME?)" if not (isinstance(c.got, tuple) and c.got[0] == "error" and c.got[1] != 525) \
            else "does not parse (%s)" % describe(c.got)
        c.shown = describe(c.got) if isinstance(c.got, tuple) else "(array)"
        return
    if c.spill:
        exp_rows = exp[1]
        shown, ok = [], True
        for i, row in enumerate(exp_rows):
            srow = []
            for j, e in enumerate(row):
                g = c.got[i][j]
                okv, s = judge_value(g, e, c.tol)
                ok &= okv
                srow.append(s)
            shown.append(",".join(srow))
        c.shown = "{" + ";".join(shown) + "}"
        c.status, c.note = ("match", "") if ok else ("differs", "")
    else:
        c.shown = describe(c.got)
        if isinstance(exp, tuple) and exp[0] in ("any", "anyerr", "anynum"):
            if c.got[0] == "error" and c.got[1] == 525:
                c.status, c.note = "missing", "not known (#NAME?)"
            elif exp[0] == "anyerr" or (c.got[0] != "error" and (exp[0] == "any" or c.got[0] == "num")):
                c.status, c.note = "match", "recognised"
            else:
                c.status, c.note = "differs", ""
        else:
            ok, _ = judge_value(c.got, exp, c.tol)
            if c.got[0] == "error" and c.got[1] == 525:
                c.status, c.note = "missing", "not known (#NAME?)"
            elif ok:
                c.status, c.note = "match", ""
            else:
                c.status, c.note = "differs", ""
                if c.got[0] == "error" and not c.got[2].startswith("#"):
                    c.note = "shows %s%s" % (c.got[2], ", saved as %s" % c.saved_error if c.saved_error else "")
                    if c.saved_error == describe_expected(exp):
                        c.note = "shows %s instead of %s (saved as %s)" % (c.got[2], c.saved_error, c.saved_error)
                elif c.got[0] == "num" and isinstance(exp, bool) and (c.got[1] != 0) == exp:
                    c.note = "right value, shown as %s instead of %s" % (general_num(c.got[1]), describe_expected(exp))
                elif c.got[0] == "bool" and isinstance(exp, float) and close_enough(c.got[2], exp, c.tol):
                    c.note = "right value (%s), shown as %s" % (general_num(c.got[2]), describe(c.got))
    # the round trip: every prefixed name must come back as Excel spells it
    if c.status == "match":
        want = set(re.findall(r"_xlfn\.(?:_xlws\.)?[A-Z0-9.]+(?=\()", c.file_formula))
        got = set(re.findall(r"_xlfn\.(?:_xlws\.)?[A-Z0-9.]+(?=\()", c.exported or ""))
        bad = re.search(r"\b(ORG|COM)\.[A-Z]", c.exported or "")
        if want - got or bad:
            c.status = "export"
            c.note = "saved as: %s" % (c.exported or "(no formula)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xlsx", default=os.path.join(os.environ.get("TMPDIR", "/var/tmp"),
                                                   "sgoffice-corpus-%d.xlsx" % os.getpid()))
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--json")
    ap.add_argument("--report")
    ap.add_argument("--baseline")
    ap.add_argument("--profile")
    ap.add_argument("--oxt")
    ap.add_argument("--xcd")
    ap.add_argument("--seed", help="the registrymodifications.xcu a new user's profile starts with")
    ap.add_argument("--wine", help="the soffice.exe of the LibreOffice Get SG Office installed, run under $WINE")
    ap.add_argument("--bundled", help="a directory of unpacked extensions, read as bundled ones")
    ap.add_argument("--only", help="a regular expression over case ids")
    ap.add_argument("--engine", default="", help="what ran it, for the report")
    ap.add_argument("--stock", action="store_true",
                    help="also run on LibreOffice as Debian ships it (no SG Office defaults or add-in), for the report")
    a = ap.parse_args()
    cases = load_corpus(sorted(glob.glob(os.path.join(HERE, "corpus-*.txt"))))
    if a.only:
        cases = [c for c in cases if re.search(a.only, c.id)]
    build(cases, a.xlsx)
    print("corpus: %d cases, %d functions -> %s" % (len(cases), len({c.name for c in cases}), a.xlsx))
    if not a.run:
        return 0
    out = a.xlsx.replace(".xlsx", "-saved.xlsx")
    if a.stock:
        run(cases, a.xlsx, out)
        for c in cases:
            judge(c)
            c.stock = c.status
    run(cases, a.xlsx, out, profile=a.profile, oxt=a.oxt, xcd=a.xcd, bundled=a.bundled, wine=a.wine, seed=a.seed)
    for c in cases:
        judge(c)
    res = {c.id: {"name": c.name, "category": c.category, "formula": c.formula, "file": c.file_formula,
                  "expected": describe_expected(c.expected), "got": c.shown, "status": c.status, "note": c.note,
                  "stock": getattr(c, "stock", None)}
           for c in cases}
    counts = {}
    for c in cases:
        counts[c.status] = counts.get(c.status, 0) + 1
    print("results: " + ", ".join("%s %d" % kv for kv in sorted(counts.items())))
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1, ensure_ascii=False)
    if a.report:
        import report
        report.write(a.report, cases, a.engine)
    rc = 0
    if a.baseline:
        base = json.load(open(a.baseline))
        regress = [k for k, v in base.items() if v["status"] == "match" and res.get(k, {}).get("status") != "match"]
        better = [k for k, v in res.items() if v["status"] == "match" and base.get(k, {}).get("status") != "match"]
        for k in regress:
            print("REGRESSION  %s  %s  expected %s, got %s (%s)" % (k, res.get(k, {}).get("file", "?"),
                  res.get(k, {}).get("expected"), res.get(k, {}).get("got"), res.get(k, {}).get("status")))
        for k in better:
            print("better      %s  now matches Excel" % k)
        print("baseline: %d regressions, %d improvements" % (len(regress), len(better)))
        rc = 1 if regress else 0
    return rc


if __name__ == "__main__":
    sys.exit(main())
