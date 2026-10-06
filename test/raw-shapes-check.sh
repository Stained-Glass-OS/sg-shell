#!/bin/sh
# Gate: the shell's programs draw round and slanted shapes soft-edged. GDI's
# Ellipse, RoundRect, Polygon, Polyline, Arc, AngleArc, Pie, Chord and
# PolyBezier have no antialiasing: drawn straight on a window they give
# stepped edges ("low quality", David 2026-10-05/06). Every such call in src/
# must be one of:
#
#   - sg-smooth.h's drop-in (sg_ellipse, sg_round_rect, sg_polygon, ...)
#   - inside a function drawn larger and averaged down: an sg_smooth art
#     callback (HDC dc, int w, int h, const void *arg), or a function whose
#     header has a "sg-smooth:" comment on the line above it (drawn in an
#     sg_ss region, or on a canvas the function itself supersamples)
#   - on a line marked "sg-smooth:" (with the reason it may be jagged)
#   - in a file whose opening comment says "sg-smooth:" (all of it drawn
#     supersampled, or a picture's own pixels, as Paint's canvas)
#
# docs/icons.md lists each glyph and how it is drawn.
# Mutant: a raw RoundRect added back to a program's button fails it
# (SG_RAW_SHAPES_EXTRA=FILE adds a file to scan).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
python3 - "$HERE" ${SG_RAW_SHAPES_EXTRA:-} <<'PY'
import glob, os, re, sys
here = sys.argv[1]
files = sorted(glob.glob(os.path.join(here, "src", "**", "*.c"), recursive=True)) + sys.argv[2:]
call = re.compile(r"(?<![\w.>])(Ellipse|RoundRect|Polygon|Polyline|PolyPolygon|PolyPolyline|Arc|ArcTo|AngleArc|Pie|Chord|PolyBezier|PolyBezierTo)\s*\(")
head = re.compile(r"^[A-Za-z_][^;]*\(")
art = re.compile(r"\(\s*HDC\s+\w+\s*,\s*int\s+\w+\s*,\s*int\s+\w+\s*,\s*(const\s+)?void\s*\*\s*\w+\s*\)")
bad, n_ok, n_calls = [], 0, 0
for f in files:
    lines = open(f, errors="replace").read().split("\n")
    if any("sg-smooth:" in l for l in lines[:60] if l.lstrip().startswith(("/*", "*", "//"))):
        continue
    fn_ok, fn = False, None
    for i, l in enumerate(lines):
        if head.match(l) and not l.rstrip().endswith(";") and not l.startswith(("typedef", "struct", "enum")):
            fn = l.split("(")[0].split()[-1].lstrip("*")
            prev = lines[i - 1] if i else ""
            fn_ok = "sg-smooth:" in prev or bool(art.search(l))
        if l.startswith("}"):
            fn_ok, fn = False, None
        stripped = re.sub(r'"[^"]*"', '""', l.split("//")[0])
        if stripped.lstrip().startswith(("*", "/*")):
            continue
        for m in call.finditer(stripped):
            n_calls += 1
            prev = lines[i - 1] if i else ""
            if fn_ok or "sg-smooth:" in l or ("sg-smooth:" in prev and prev.strip().startswith("/*")):
                n_ok += 1
                continue
            bad.append("%s:%d (%s): %s" % (os.path.relpath(f, here), i + 1, fn or "?", l.strip()[:100]))
if bad:
    print("FAIL  raw GDI shapes (stepped edges): use sg-smooth.h's drop-ins, or mark why not:\n      " + "\n      ".join(bad))
    sys.exit(1)
print("PASS  every round or slanted shape in src/ is drawn smooth or marked (%d calls in supersampled art)" % n_ok)
PY
