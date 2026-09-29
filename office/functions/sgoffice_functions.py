"""SG Office Functions -- Excel's functions that LibreOffice's engine lacks,
as a Calc add-in: the logic (component.py is the UNO side, sgoffice_spec.py
the list, build-oxt.py the packaging).

Values arrive as LibreOffice passes them to an add-in: a number (a logical
value is a number too), a string, None for an empty cell or a left-out
argument, or a tuple of rows for a range or an array. An error result is a
NaN carrying LibreOffice's error code (as the engine encodes errors), which
works both as a single result and inside an array.

Plain Python, so the gate can test it without LibreOffice
(office/functions/test_functions.py).

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import cmath
import math
import re
import struct
import unicodedata

# ---- errors -------------------------------------------------------------------------------------


def _err(code):
    return struct.unpack("<d", struct.pack("<Q", 0x7FF8000000000000 | code))[0]


NA, VALUE, NUM, DIV0 = _err(32767), _err(519), _err(503), _err(532)
CALC = VALUE          # Excel's #CALC! (an empty result) has no LibreOffice equivalent


def is_error(v):
    return isinstance(v, float) and math.isnan(v)


def error_code(v):
    return struct.unpack("<Q", struct.pack("<d", v))[0] & 0xFFFF


class Fail(Exception):
    def __init__(self, err):
        Exception.__init__(self)
        self.err = err


# ---- values -------------------------------------------------------------------------------------


def matrix(v):
    """Any argument as a list of rows."""
    if isinstance(v, tuple):
        if v and isinstance(v[0], tuple):
            return [list(r) for r in v]
        return [list(v)]
    return [[v]]


def scalar(v):
    if isinstance(v, tuple):
        m = matrix(v)
        return m[0][0] if m and m[0] else None
    return v


def general(x):
    """A number as Excel's General format writes it into text."""
    if x == 0:
        return "0"
    if x == int(x) and abs(x) < 1e15:
        return str(int(x))
    a = abs(x)
    if 1e-9 <= a < 1e15:
        s = "%.15g" % x
        if "e" in s:
            s = "%.*f" % (max(0, 14 - int(math.floor(math.log10(a)))), x)
        if "." in s:
            s = s.rstrip("0").rstrip(".")
        return s
    m, e = ("%.14E" % x).split("E")
    m = m.rstrip("0").rstrip(".")
    return "%sE%s%02d" % (m, "-" if int(e) < 0 else "+", abs(int(e)))


def text(v):
    v = scalar(v)
    if v is None:
        return ""
    if isinstance(v, str):
        return v
    if is_error(v):
        raise Fail(v)
    return general(v)


def number(v, default=None):
    v = scalar(v)
    if v is None:
        if default is None:
            raise Fail(VALUE)
        return default
    if isinstance(v, str):
        try:
            return float(v.strip())
        except ValueError:
            raise Fail(VALUE)
    if is_error(v):
        raise Fail(v)
    return float(v)


def integer(v, default=None):
    return int(number(v, default))


def flag(v, default=False):
    return bool(number(v, 1.0 if default else 0.0))


def strings(v):
    """A delimiter argument: one string or an array of them."""
    return [text(x) for row in matrix(v) for x in row if x is not None]


def blank(v):
    """An empty cell: LibreOffice hands an add-in an empty cell of a range as
    an empty string, so a formula's "" reads as empty too."""
    return v is None or v == ""


def cell_out(v):
    """A value for an output array: an empty cell reads as 0, as in Excel."""
    return 0.0 if v is None else v


def rect(rows, pad):
    width = max((len(r) for r in rows), default=0)
    return tuple(tuple(r + [pad] * (width - len(r))) for r in rows)


def pad_value(v):
    v = scalar(v)
    return NA if v is None else v


def guard(fn):
    """Excel's errors out of Fail; unexpected trouble is #VALUE!."""
    def run(*args, array=False):
        try:
            return fn(*args)
        except Fail as f:
            return ((f.err,),) if array else f.err
        except (ValueError, TypeError, IndexError, ZeroDivisionError, OverflowError, re.error):
            return ((VALUE,),) if array else VALUE
    run.__name__ = fn.__name__
    return run


# ---- text ---------------------------------------------------------------------------------------


def _occurrences(s, delims, ci):
    t = s.lower() if ci else s
    ds = [d.lower() if ci else d for d in delims]
    out, i = [], 0
    while i <= len(t):
        best = None
        for d in ds:
            j = t.find(d, i)
            if j >= 0 and (best is None or j < best[0] or (j == best[0] and len(d) > best[1] - best[0])):
                best = (j, j + len(d))
        if best is None:
            break
        out.append(best)
        i = best[1] if best[1] > best[0] else best[0] + 1
    return out


def _before_after(after, s, delimiter, instance, match_mode, match_end, if_not_found):
    s = text(s)
    delims = strings(delimiter)
    if not delims:
        raise Fail(VALUE)
    n = integer(instance, 1.0)
    if n == 0 or abs(n) > max(len(s), 1):
        raise Fail(VALUE)
    occ = _occurrences(s, delims, integer(match_mode, 0.0) == 1)
    if flag(match_end):
        occ = occ + [(len(s), len(s))] if n > 0 else [(0, 0)] + occ
    if abs(n) > len(occ):
        if if_not_found is not None:
            return scalar(if_not_found)
        raise Fail(NA)
    o = occ[n - 1] if n > 0 else occ[n]
    return s[o[1]:] if after else s[:o[0]]


@guard
def textbefore(s, delimiter, instance=None, match_mode=None, match_end=None, if_not_found=None):
    return _before_after(False, s, delimiter, instance, match_mode, match_end, if_not_found)


@guard
def textafter(s, delimiter, instance=None, match_mode=None, match_end=None, if_not_found=None):
    return _before_after(True, s, delimiter, instance, match_mode, match_end, if_not_found)


def _split(s, delims, ci):
    delims = [d for d in delims if d]
    if not delims:
        return [s]
    pat = "|".join(re.escape(d) for d in sorted(delims, key=len, reverse=True))
    return re.split(pat, s, flags=re.I if ci else 0)


@guard
def textsplit(s, col_delimiter=None, row_delimiter=None, ignore_empty=None, match_mode=None, pad_with=None):
    s = text(s)
    cols = strings(col_delimiter) if col_delimiter is not None else []
    rows_d = strings(row_delimiter) if row_delimiter is not None else []
    if not cols and not rows_d:
        raise Fail(VALUE)
    ci = integer(match_mode, 0.0) == 1
    skip = flag(ignore_empty)
    rows = _split(s, rows_d, ci) if rows_d else [s]
    if skip:
        rows = [r for r in rows if r != ""]
    out = []
    for r in rows:
        cells = _split(r, cols, ci) if cols else [r]
        if skip:
            cells = [c for c in cells if c != ""]
        out.append(cells)
    if not out or not any(out):
        raise Fail(CALC)
    return rect(out, pad_value(pad_with))


def _value_text(v, strict):
    if v is None:
        return ""
    if isinstance(v, str):
        return '"%s"' % v.replace('"', '""') if strict else v
    if is_error(v):
        return {32767: "#N/A", 519: "#VALUE!", 503: "#NUM!", 532: "#DIV/0!", 524: "#REF!", 525: "#NAME?",
                521: "#NULL!"}.get(error_code(v), "#VALUE!")
    return general(v)


@guard
def arraytotext(array, fmt=None):
    strict = integer(fmt, 0.0) == 1
    rows = matrix(array)
    if strict:
        return "{" + ";".join(",".join(_value_text(v, True) for v in r) for r in rows) + "}"
    return ", ".join(_value_text(v, False) for r in rows for v in r)


@guard
def valuetotext(value, fmt=None):
    return _value_text(scalar(value), integer(fmt, 0.0) == 1)


def _regex(pattern, case):
    ci = integer(case, 0.0) == 1
    return re.compile(text(pattern), re.I if ci else 0)


@guard
def regextest(s, pattern, case=None):
    return 1.0 if _regex(pattern, case).search(text(s)) else 0.0


@guard
def regexextract(s, pattern, mode=None, case=None):
    rx, s, mode = _regex(pattern, case), text(s), integer(mode, 0.0)
    if mode == 0:
        m = rx.search(s)
        if not m:
            raise Fail(NA)
        return ((m.group(0),),)
    if mode == 1:
        found = [m.group(0) for m in rx.finditer(s)]
        if not found:
            raise Fail(NA)
        return tuple((f,) for f in found)
    if mode == 2:
        m = rx.search(s)
        if not m:
            raise Fail(NA)
        groups = m.groups() or (m.group(0),)
        return (tuple("" if g is None else g for g in groups),)
    raise Fail(VALUE)


@guard
def regexreplace(s, pattern, replacement, occurrence=None, case=None):
    rx, s = _regex(pattern, case), text(s)
    rep = re.sub(r"\$(\d+)|\$\{(\d+)\}", lambda m: "\\g<%s>" % (m.group(1) or m.group(2)),
                 text(replacement).replace("\\", "\\\\"))
    n = integer(occurrence, 0.0)
    if n == 0:
        return rx.sub(rep, s)
    matches = list(rx.finditer(s))
    if abs(n) > len(matches):
        return s
    m = matches[n - 1] if n > 0 else matches[n]
    return s[:m.start()] + m.expand(rep) + s[m.end():]


@guard
def dbcs(s):
    out = []
    for ch in text(s):
        o = ord(ch)
        if 0x21 <= o <= 0x7E:
            out.append(chr(o + 0xFEE0))
        elif ch == " ":
            out.append("　")
        elif 0xFF61 <= o <= 0xFF9F:
            out.append(unicodedata.normalize("NFKC", ch))
        else:
            out.append(ch)
    # half-width voicing marks join the kana before them, as NFKC composes them
    return unicodedata.normalize("NFC", "".join(out))


@guard
def phonetic(reference):
    return "".join(text(v) for r in matrix(reference) for v in r)


# ---- arrays -------------------------------------------------------------------------------------


def _args(first, rest):
    return [first] + list(rest or ())


@guard
def vstack(first, rest=()):
    rows = []
    for a in _args(first, rest):
        rows += [[cell_out(v) for v in r] for r in matrix(a)]
    return rect(rows, NA)


@guard
def hstack(first, rest=()):
    blocks = [[[cell_out(v) for v in r] for r in matrix(a)] for a in _args(first, rest)]
    height = max(len(b) for b in blocks)
    out = [[] for _ in range(height)]
    for b in blocks:
        width = max(len(r) for r in b)
        for i in range(height):
            out[i] += (b[i] + [NA] * (width - len(b[i]))) if i < len(b) else [NA] * width
    return tuple(tuple(r) for r in out)


def _flat(array, ignore, by_col):
    rows = matrix(array)
    ignore = integer(ignore, 0.0)
    if by_col:
        vals = [rows[i][j] for j in range(max(len(r) for r in rows)) for i in range(len(rows)) if j < len(rows[i])]
    else:
        vals = [v for r in rows for v in r]
    if ignore in (1, 3):
        vals = [v for v in vals if not blank(v)]
    if ignore in (2, 3):
        vals = [v for v in vals if not is_error(v)]
    if ignore not in (0, 1, 2, 3):
        raise Fail(VALUE)
    if not vals:
        raise Fail(CALC)
    return [cell_out(v) for v in vals]


@guard
def torow(array, ignore=None, scan_by_column=None):
    return (tuple(_flat(array, ignore, flag(scan_by_column))),)


@guard
def tocol(array, ignore=None, scan_by_column=None):
    return tuple((v,) for v in _flat(array, ignore, flag(scan_by_column)))


def _vector(v):
    rows = matrix(v)
    if len(rows) > 1 and any(len(r) > 1 for r in rows):
        raise Fail(VALUE)
    return [cell_out(x) for r in rows for x in r]


@guard
def wraprows(vector, wrap_count, pad_with=None):
    vals, n = _vector(vector), integer(wrap_count)
    if n < 1:
        raise Fail(NUM)
    return rect([vals[i:i + n] for i in range(0, len(vals), n)], pad_value(pad_with))


@guard
def wrapcols(vector, wrap_count, pad_with=None):
    vals, n = _vector(vector), integer(wrap_count)
    if n < 1:
        raise Fail(NUM)
    pad = pad_value(pad_with)
    cols = [vals[i:i + n] for i in range(0, len(vals), n)]
    cols = [c + [pad] * (n - len(c)) for c in cols]
    return tuple(tuple(c[i] for c in cols) for i in range(n))


def _grid(array):
    rows = [[cell_out(v) for v in r] for r in matrix(array)]
    return rect(rows, NA)


def _slice(n, count, take):
    if n is None:
        return 0, count
    n = int(number(n))
    if take:
        if n == 0:
            raise Fail(CALC)
        return (0, min(n, count)) if n > 0 else (max(0, count + n), count)
    return (min(n, count), count) if n >= 0 else (0, max(0, count + n))


def _take_drop(array, rows, columns, take):
    g = _grid(array)
    r0, r1 = _slice(scalar(rows), len(g), take)
    c0, c1 = _slice(scalar(columns), len(g[0]) if g else 0, take)
    out = tuple(tuple(r[c0:c1]) for r in g[r0:r1])
    if not out or not out[0]:
        raise Fail(CALC)
    return out


@guard
def take(array, rows=None, columns=None):
    return _take_drop(array, rows, columns, True)


@guard
def drop(array, rows=None, columns=None):
    return _take_drop(array, rows, columns, False)


def _indices(first, rest, count):
    out = []
    for a in _args(first, rest):
        for r in matrix(a):
            for v in r:
                i = int(number(v))
                if i == 0 or abs(i) > count:
                    raise Fail(VALUE)
                out.append(i - 1 if i > 0 else count + i)
    return out


@guard
def chooserows(array, first, rest=()):
    g = _grid(array)
    return tuple(g[i] for i in _indices(first, rest, len(g)))


@guard
def choosecols(array, first, rest=()):
    g = _grid(array)
    idx = _indices(first, rest, len(g[0]))
    return tuple(tuple(r[i] for i in idx) for r in g)


@guard
def expand(array, rows, columns=None, pad_with=None):
    g = [list(r) for r in _grid(array)]
    nr = int(number(rows)) if scalar(rows) is not None else len(g)
    nc = int(number(columns)) if scalar(columns) is not None else len(g[0])
    if nr < len(g) or nc < len(g[0]):
        raise Fail(VALUE)
    pad = pad_value(pad_with)
    g = [r + [pad] * (nc - len(r)) for r in g] + [[pad] * nc for _ in range(nr - len(g))]
    return tuple(tuple(r) for r in g)


@guard
def trimrange(rng, trim_rows=None, trim_cols=None):
    rows = matrix(rng)
    tr, tc = integer(trim_rows, 3.0), integer(trim_cols, 3.0)

    def blank_row(r):
        return all(blank(v) for v in r)

    def blank_col(j):
        return all(j >= len(r) or blank(r[j]) for r in rows)
    top, bottom = 0, len(rows)
    if tr in (1, 3):
        while top < bottom and blank_row(rows[top]):
            top += 1
    if tr in (2, 3):
        while bottom > top and blank_row(rows[bottom - 1]):
            bottom -= 1
    width = max(len(r) for r in rows)
    left, right = 0, width
    if tc in (1, 3):
        while left < right and blank_col(left):
            left += 1
    if tc in (2, 3):
        while right > left and blank_col(right - 1):
            right -= 1
    out = [[cell_out(v) for v in r[left:right]] for r in rows[top:bottom]]
    if not out or not out[0]:
        raise Fail(CALC)
    return rect(out, 0.0)


# ---- math, statistics, engineering ------------------------------------------------------------------


def _sum(v):
    total = 0.0
    for r in matrix(v):
        for x in r:
            if is_error(x):
                raise Fail(x)
            if isinstance(x, float) or isinstance(x, int):
                total += x
    return total


@guard
def percentof(subset, whole):
    w = _sum(whole)
    if w == 0:
        raise Fail(DIV0)
    return _sum(subset) / w


@guard
def binomdistrange(trials, p, s, s2=None):
    n, p, a = int(number(trials)), number(p), int(number(s))
    b = int(number(s2)) if scalar(s2) is not None else a
    if n < 0 or not 0 <= p <= 1 or a < 0 or a > n or b < a or b > n:
        raise Fail(NUM)
    return math.fsum(math.comb(n, k) * p ** k * (1 - p) ** (n - k) for k in range(a, b + 1))


_COMPLEX = re.compile(r"^([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)?(?:([+-]?(?:\d+\.?\d*|\.\d+)?(?:[eE][+-]?\d+)?)([ij]))?$")


def parse_complex(v):
    v = scalar(v)
    if v is None:
        return 0j, "i"
    if not isinstance(v, str):
        return complex(number(v), 0), "i"
    s = v.strip()
    m = _COMPLEX.match(s)
    if not s or not m or (m.group(1) is None and m.group(3) is None):
        raise Fail(NUM)
    re_part = float(m.group(1)) if m.group(1) else 0.0
    if m.group(3):
        coeff = m.group(2)
        im = 1.0 if coeff in ("", "+") else -1.0 if coeff == "-" else float(coeff)
        # "3+4i": the regex gives real "3" and imaginary "+4"; "4i" alone: no real part
        return complex(re_part, im), m.group(3)
    return complex(re_part, 0), "i"


def format_complex(z, suffix="i"):
    def clean(x):
        return 0.0 if abs(x) < 1e-300 else float("%.15g" % x)
    a, b = clean(z.real), clean(z.imag)
    if b == 0:
        return general(a)
    im = suffix if b == 1 else "-" + suffix if b == -1 else general(b) + suffix
    if a == 0:
        return im
    return general(a) + ("" if im.startswith("-") else "+") + im


def _im(fn):
    @guard
    def run(z):
        c, suffix = parse_complex(z)
        try:
            return format_complex(fn(c), suffix)
        except (ZeroDivisionError, ValueError, OverflowError):
            raise Fail(NUM)
    return run


imcosh = _im(cmath.cosh)
imcot = _im(lambda z: 1 / cmath.tan(z))
imcsc = _im(lambda z: 1 / cmath.sin(z))
imcsch = _im(lambda z: 1 / cmath.sinh(z))
imsec = _im(lambda z: 1 / cmath.cos(z))
imsech = _im(lambda z: 1 / cmath.cosh(z))
imsinh = _im(cmath.sinh)
imtan = _im(cmath.tan)
