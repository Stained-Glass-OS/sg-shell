#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate: every icon the build makes has the frames Windows programs ask for at
# 100-250% display scale. Runs each icon generator the Makefile names (the
# `python3 <script> $(BUILD)/...` lines, so a new one is covered by being
# added there) into a scratch build directory, then checks:
#
#   - every .ico has the 16, 20, 24, 32, 40, 48, 64, 96, 128 and 256 px frames
#     (tools/sgicon.py SIZES), the 256 px one PNG-compressed, the others 32-bit
#     DIBs, none of them empty
#   - every ICON a .rc file names is one a generator makes
#   - the consoles' picture strips exist at 16, 20, 24, 32 and 40 px
#   - SG Office's Linux icons are in hicolor at 16-512 px
#
# Mutant: SG_MUTANT_ICON_SIZES=1 (tools/sgicon.py writes the old six sizes) fails it.
# Needs python3 with PIL; skips (77) without it.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
python3 -c 'import PIL' 2>/dev/null || { echo "SKIP: python3-pil missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-icon-sizes.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/build/office/icons"

# the generators, as the Makefile runs them
sed -n 's/^\t@\?python3 \(src\/[^ ]*\(icon\|icons\)[^ ]*\.py\) \(.*\)$/\1 \3/p' "$HERE/Makefile" | sed 's/ \\$//' > "$T/gens"
[ -s "$T/gens" ] || { fail "no icon generators found in the Makefile"; exit 1; }
n=0
while read -r script args; do
    # continuation lines (mmc's) are joined: take the next line's paths too
    case "$script" in src/mmc/gen-icons.py)
        args="$args $(sed -n '/src\/mmc\/gen-icons.py/{n;p}' "$HERE/Makefile" | sed 's/^[[:space:]]*//')";;
    esac
    # shellcheck disable=SC2086
    set -- $(echo "$args" | sed "s|\$(BUILD)|$T/build|g")
    (cd "$HERE" && python3 "$script" "$@") > "$T/gen.log" 2>&1 || fail "$script failed: $(tail -2 "$T/gen.log")"
    n=$((n + 1))
done < "$T/gens"
(cd "$HERE" && python3 office/gen-icons.py "$T/build/office/icons") > "$T/gen.log" 2>&1 || fail "office/gen-icons.py failed: $(tail -2 "$T/gen.log")"
pass "ran $n icon generators and SG Office's"

python3 - "$HERE" "$T/build" <<'PY' || RC=1
import glob, os, re, sys
here, build = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(here, "tools"))
import sgicon
from PIL import Image
want = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
icos = sorted(glob.glob(os.path.join(build, "**", "*.ico"), recursive=True))
bad = []
for p in icos:
    fr = sgicon.read_ico_sizes(p)
    sizes = sorted(w for w, h, f in fr)
    if sizes != want:
        bad.append("%s: %s" % (os.path.relpath(p, build), sizes))
        continue
    for w, h, f in fr:
        if (w == 256) != (f == "png"):
            bad.append("%s: %d px frame is %s" % (os.path.relpath(p, build), w, f))
    im = Image.open(p)
    for s in want:
        try:
            im.size = (s, s)
            fr_im = im.copy().convert("RGBA")
        except Exception as e:
            bad.append("%s: %d px frame unreadable (%s)" % (os.path.relpath(p, build), s, e))
            continue
        if fr_im.getchannel("A").getextrema()[1] == 0:
            bad.append("%s: %d px frame is empty" % (os.path.relpath(p, build), s))
if bad:
    print("FAIL  icons without every frame:\n      " + "\n      ".join(bad))
    sys.exit(1)
print("PASS  %d .ico files, each with %s px frames (256 PNG, the rest 32-bit DIB)" % (len(icos), ",".join(map(str, want))))

made = {os.path.basename(p) for p in icos}
missing = []
for rc in glob.glob(os.path.join(here, "src", "**", "*.rc"), recursive=True):
    for m in re.finditer(r'^\s*\w+\s+ICON\s+"([^"]+)"', open(rc, errors="replace").read(), re.M):
        if m.group(1) not in made:
            missing.append("%s: %s" % (os.path.relpath(rc, here), m.group(1)))
if missing:
    print("FAIL  ICONs no generator makes:\n      " + "\n      ".join(missing))
    sys.exit(1)
print("PASS  every ICON the .rc files name is drawn by a generator")

strips = {n: os.path.join(build, "mmc%d.bmp" % n) for n in (16, 20, 24, 32, 40)}
bad = [n for n, p in strips.items() if not os.path.exists(p) or Image.open(p).size[1] != n]
if bad:
    print("FAIL  the consoles' picture strips missing at %s px" % bad)
    sys.exit(1)
print("PASS  the consoles' picture strips at 16, 20, 24, 32 and 40 px")

hic = [16, 22, 24, 32, 48, 64, 96, 128, 256, 512]
miss = [n for n in hic for k in ("documents", "spreadsheets", "presentations")
        if not os.path.exists(os.path.join(build, "office", "icons", "png", "%dx%d" % (n, n), "apps", "sg-office-%s.png" % k))]
if miss:
    print("FAIL  SG Office's hicolor icons missing at %s px" % sorted(set(miss)))
    sys.exit(1)
print("PASS  SG Office's Linux icons in hicolor at %s px" % ",".join(map(str, hic)))
PY
[ $RC = 0 ] && echo "icon-sizes-check: ok" || echo "icon-sizes-check: FAILED"
exit $RC
