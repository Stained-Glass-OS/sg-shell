#!/bin/sh
# Gate for Compressed (zipped) Folders (sg-zip): our own inflate, deflate and
# ZIP container against Python's zipfile (and Info-ZIP's unzip when present),
# the zip-slip refusals, a damaged entry caught by its CRC, and the windows --
# the browse window opened through the .zip association, navigated with the
# keyboard, and the Extract wizard driven with xdotool, including its
# Replace-or-Skip prompt.
#
# Screenshots: build/zip-{browse,browse-sub,wizard,ask}.png. Needs wine-sg,
# Xvfb, xdotool, ImageMagick and python3; skips (77) without them.
# SG_ZIP_EXE runs another build (the mutation test does).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_ZIP_EXE:-$HERE/build/sg-zip64.exe}"
OUT="$HERE/build"
RC=0; DPY=115; XP=""
pass() { printf "PASS  %s\n" "$*"; }
fail() { printf "FAIL  %s\n" "$*"; RC=1; }

for need in Xvfb xdotool import python3; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-zip-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH" SG_ZIP_NO_SHOW=1
wine wineboot --init >/dev/null 2>&1; wineserver -w
C="$WINEPREFIX/drive_c"
cp "$EXE" "$C/sg-zip64.exe"
Z() { wine 'C:\sg-zip64.exe' "$@" >/dev/null 2>&1; }

# --- fixtures, by Python's zipfile ---------------------------------------------------------------
mkdir -p "$C/fx"
( cd "$C/fx" && python3 - <<'EOF'
import os, random, zipfile
random.seed(7)
os.makedirs("src/docs/deep er/deepest", exist_ok=True)
os.makedirs("src/empty folder", exist_ok=True)
words = "stained glass lead pane window light colour rose".split()
open("src/text.txt", "w").write(" ".join(random.choice(words) for _ in range(200000)))  # ~1 MB, compressible
open("src/random.bin", "wb").write(random.randbytes(300000))
open("src/empty.dat", "wb").close()
open("src/docs/caf\u00e9 \u65e5\u672c \u2013 notes.txt", "w", encoding="utf-8").write("unicode \u00e9\u00e8\n" * 50)
open("src/docs/deep er/deepest/leaf.txt", "w").write("leaf\n")
with zipfile.ZipFile("mixed.zip", "w") as z:
    for root, dirs, files in sorted(os.walk("src")):
        rel = os.path.relpath(root, "src")
        if rel != "." and not files and not dirs:
            z.write(root, rel + "/")
        for f in sorted(files):
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, "src"),
                    compress_type=zipfile.ZIP_STORED if f.endswith(".bin") or f == "leaf.txt" else zipfile.ZIP_DEFLATED,
                    compresslevel=9 if f == "text.txt" else None)
# hostile names
with zipfile.ZipFile("slip.zip", "w") as z:
    for name in ["ok.txt", "../evil.txt", "a/../../up.txt", "..\\back.txt", "C:/drive.txt", "/abs.txt", "sub/.../dots.txt", "x/y:stream.txt"]:
        zi = zipfile.ZipInfo("placeholder")
        zi.filename = name      # as written, not as ZipInfo would clean it
        z.writestr(zi, "payload " + name)
# a damaged entry: one byte of a stored file changed after the CRC was taken
with zipfile.ZipFile("bad.zip", "w") as z:
    z.writestr("good.txt", "fine")
    z.writestr("bad.txt", "THIS PAYLOAD IS DAMAGED")
data = bytearray(open("bad.zip", "rb").read())
i = data.index(b"THIS PAYLOAD")
data[i] ^= 0x20
open("bad.zip", "wb").write(bytes(data))
EOF
)

# --- 1. extraction of Python's zips is byte-identical ------------------------------------------------
Z /extract 'C:\fx\mixed.zip' 'C:\fx\out' /quiet /log 'C:\fx\x.log'; x=$?
if [ $x = 0 ] && diff -r "$C/fx/src" "$C/fx/out" >/dev/null 2>&1; then
    pass "extract: deflate (level 9), stored, nested, unicode, empty file and folder identical"
else
    fail "extract differs (exit $x)"; diff -rq "$C/fx/src" "$C/fx/out" 2>&1 | head -5
fi
grep -q "written=5 errors=0 refused=0" "$C/fx/x.log" && pass "extract log: 5 written, none refused" \
    || fail "extract log: $(tail -1 "$C/fx/x.log")"
# the stored modification time survives (to the DOS format's two seconds)
python3 - "$C/fx/src/text.txt" "$C/fx/out/text.txt" <<'EOF' && pass "extracted file keeps its date" || fail "extracted file's date is not the archive's"
import os, sys
a, b = (os.path.getmtime(p) for p in sys.argv[1:])
sys.exit(0 if abs(a - b) <= 2 else 1)
EOF
# /skip leaves an existing file; /overwrite replaces it
echo changed > "$C/fx/out/empty.dat"
Z /extract 'C:\fx\mixed.zip' 'C:\fx\out' /skip /quiet /log 'C:\fx\s.log'
[ "$(cat "$C/fx/out/empty.dat")" = changed ] && grep -q "skipped=5" "$C/fx/s.log" && pass "/skip keeps existing files" || fail "/skip: $(tail -1 "$C/fx/s.log")"
Z /extract 'C:\fx\mixed.zip' 'C:\fx\out' /overwrite /quiet
[ ! -s "$C/fx/out/empty.dat" ] && pass "/overwrite replaces them" || fail "/overwrite left the old file"

# --- 2. zip-slip: every hostile name refused, nothing outside the destination -----------------------------
mkdir -p "$C/slip/in"
Z /extract 'C:\fx\slip.zip' 'C:\slip\in\out' /quiet /log 'C:\slip\x.log'; x=$?
leaked=$(find "$C" -path "$C/windows" -prune -o \( -name 'evil.txt' -o -name 'up.txt' -o -name 'back.txt' -o -name 'drive.txt' -o -name 'abs.txt' \
         -o -name 'dots.txt' -o -name 'y' -o -name 'y:stream.txt' -o -name 'stream.txt' \) -print || true)
if [ -z "$leaked" ] && [ -f "$C/slip/in/out/ok.txt" ] && [ $x = 1 ] && grep -q "refused=7" "$C/slip/x.log"; then
    pass "zip-slip: 7 hostile names refused, ok.txt extracted, exit 1"
else
    fail "zip-slip: exit $x, $(tail -1 "$C/slip/x.log" 2>/dev/null), leaked: $leaked"
fi

# --- 3. a damaged entry is caught by its CRC and not written ------------------------------------------
Z /extract 'C:\fx\bad.zip' 'C:\fx\bad' /quiet /log 'C:\fx\b.log'; x=$?
if [ $x = 1 ] && [ -f "$C/fx/bad/good.txt" ] && [ ! -e "$C/fx/bad/bad.txt" ] && grep -q "ERROR	bad.txt	.*checksum" "$C/fx/b.log"; then
    pass "damaged entry: CRC mismatch reported, file not written, the good one extracted"
else
    fail "damaged entry: exit $x, $(cat "$C/fx/b.log" 2>/dev/null | tr '\n' ' ')"
fi

# --- 4. our zips, checked by Python and unzip ------------------------------------------------------
Z /create 'C:\fx\src' /quiet /log 'C:\fx\c.log'; x=$?
if [ $x = 0 ] && [ -f "$C/fx/src.zip" ]; then pass "/create made src.zip beside the folder"; else fail "/create: exit $x"; fi
python3 - "$C/fx/src.zip" "$C/fx/src" <<'EOF' && pass "Python: testzip clean, every file and folder identical, UTF-8 names" || fail "Python rejects our zip"
import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); root = sys.argv[2]
assert z.testzip() is None
names = set(z.namelist())
for dirpath, dirs, files in os.walk(root):
    rel = os.path.relpath(dirpath, os.path.dirname(root)).replace(os.sep, "/")
    assert rel + "/" in names, rel
    for f in files:
        n = rel + "/" + f
        assert z.read(n) == open(os.path.join(dirpath, f), "rb").read(), n
methods = {i.filename: i.compress_type for i in z.infolist()}
assert methods["src/text.txt"] == zipfile.ZIP_DEFLATED and methods["src/random.bin"] == zipfile.ZIP_STORED, methods
ratio = z.getinfo("src/text.txt").compress_size / z.getinfo("src/text.txt").file_size
assert ratio < 0.35, ratio
print("  text.txt ratio %.3f" % ratio)
EOF
if command -v unzip >/dev/null; then
    unzip -tq "$C/fx/src.zip" >/dev/null 2>&1 && pass "unzip -t: no errors" || fail "unzip -t rejects our zip"
fi
# a single file: <name>.zip; taken names get " (2)"
Z /create 'C:\fx\src\text.txt' /quiet; Z /create 'C:\fx\src\text.txt' /quiet
[ -f "$C/fx/src/text.zip" ] && [ -f "$C/fx/src/text (2).zip" ] && pass "file -> text.zip, then text (2).zip" || fail "single-file names: $(ls "$C/fx/src")"
rm -f "$C/fx/src/text.zip" "$C/fx/src/text (2).zip"
# and our own zip round-trips through our own extractor
Z /extract 'C:\fx\src.zip' 'C:\fx\rt' /quiet && diff -r "$C/fx/src" "$C/fx/rt/src" >/dev/null 2>&1 \
    && pass "our zip round-trips through our extractor" || fail "round trip differs"

# --- 5. the windows ------------------------------------------------------------------------------
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
sed 's/Z:\\\\usr\\\\libexec\\\\stained-glass\\\\shell\\\\sg-zip64.exe/C:\\\\sg-zip64.exe/g' "$HERE/defaults/75-sg-zip.reg" > "$C/zip.reg"
wine reg import 'C:\zip.reg' >/dev/null 2>&1
wineserver -w
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
export SG_ZIP_DUMP="$C/dump.txt"
waitdump() {  # $1: a line the dump must hold
    i=0; while ! grep -qF -- "$1" "$C/dump.txt" 2>/dev/null && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
    grep -qF -- "$1" "$C/dump.txt" 2>/dev/null
}
: > "$C/dump.txt"
wine start 'C:\fx\mixed.zip' >/dev/null 2>&1 &
if waitdump "WINDOW browse" && waitdump "ROW text.txt"; then
    pass "a .zip opens (association -> CompressedFolder) in the browse window"
    grep -q "^ROW docs	File folder" "$C/dump.txt" && grep -q "^ROW empty folder	File folder" "$C/dump.txt" \
        && pass "folders listed, including an empty one" || fail "folders missing: $(grep ROW "$C/dump.txt" | tr '\n' '|')"
    row=$(grep "^ROW random.bin" "$C/dump.txt")
    echo "$row" | awk -F'\t' '$4=="No" && $6=="0%"' | grep -q . && pass "stored file: not password protected, ratio 0%" || fail "random.bin row: $row"
    row=$(grep "^ROW text.txt" "$C/dump.txt")
    echo "$row" | awk -F'\t' '{r=$6; sub("%","",r); exit !(r+0 > 60)}' && pass "deflated file: ratio $(echo "$row" | cut -f6)" || fail "text.txt row: $row"
    [ "$(grep -c '^ROW' "$C/dump.txt")" = 5 ] && pass "5 items at the top level" || fail "top level rows: $(grep -c '^ROW' "$C/dump.txt")"
else
    fail "the browse window did not open: $(cat "$C/dump.txt" 2>/dev/null)"
fi
sleep 1; import -window root "$OUT/zip-browse.png"
# keyboard: type-ahead to "docs", Enter opens it, Backspace goes up
xdotool type --delay 120 docs; sleep 0.4; xdotool key Return
if waitdump "FOLDER docs/" && waitdump "ROW deep er	File folder"; then
    pass "Enter opens a folder inside the zip"
    grep -q "^ROW café 日本 – notes.txt" "$C/dump.txt" && pass "unicode name shown" || fail "unicode row: $(grep ROW "$C/dump.txt" | tr '\n' '|')"
else fail "navigating into docs: $(head -3 "$C/dump.txt" | tr '\n' '|')"; fi
sleep 0.5; import -window root "$OUT/zip-browse-sub.png"
xdotool key BackSpace
waitdump "ROW text.txt" && grep -q "^FOLDER $" "$C/dump.txt" && pass "Backspace goes back up" || fail "Backspace: $(head -3 "$C/dump.txt" | tr '\n' '|')"
wine taskkill /im sg-zip64.exe /f >/dev/null 2>&1; sleep 1

# the wizard: type the destination, Enter extracts
: > "$C/dump.txt"
wine 'C:\sg-zip64.exe' /extractall 'C:\fx\mixed.zip' >/dev/null 2>&1 &
if waitdump "STATE ready"; then
    grep -q '^DEST C:\\fx\\mixed$' "$C/dump.txt" && pass "wizard: default destination is a folder named like the zip" \
        || fail "wizard default: $(grep DEST "$C/dump.txt")"
    sleep 1; import -window root "$OUT/zip-wizard.png"
    xdotool key ctrl+a; xdotool type --delay 80 'c:\wiz'; sleep 0.3; xdotool key Return
    if waitdump "STATE done" && diff -r "$C/fx/src" "$C/wiz" >/dev/null 2>&1; then
        pass "wizard: typed destination, Enter extracted everything identically"
    else fail "wizard extraction: $(cat "$C/dump.txt" | tr '\n' '|')"; fi
else fail "the wizard did not open"; fi
sleep 1
# again into the same place: Replace or Skip, "for all", Enter replaces
echo changed > "$C/wiz/empty.dat"
: > "$C/dump.txt"
wine 'C:\sg-zip64.exe' /extractall 'C:\fx\mixed.zip' >/dev/null 2>&1 &
if waitdump "STATE ready"; then
    xdotool key ctrl+a; xdotool type --delay 80 'c:\wiz'; sleep 0.3; xdotool key Return
    if waitdump "WINDOW ask"; then
        pass "an existing file asks Replace or Skip"
        sleep 1; import -window root "$OUT/zip-ask.png"
        # Tab to "Do this for all conflicts", Space ticks it... Enter takes the default (Replace)
        xdotool key Return
        i=0
        while [ $i -lt 20 ]; do
            grep -q "STATE done" "$C/dump.txt" && break
            grep -q "WINDOW ask" "$C/dump.txt" && { sleep 0.3; xdotool key Return; }
            sleep 0.5; i=$((i + 1))
        done
        waitdump "STATE done" && [ ! -s "$C/wiz/empty.dat" ] && pass "Replace: the file in the destination replaced" \
            || fail "after Replace: $(cat "$C/dump.txt" | tr '\n' '|'), empty.dat: $(cat "$C/wiz/empty.dat")"
    else fail "no Replace-or-Skip prompt: $(cat "$C/dump.txt" | tr '\n' '|')"; fi
else fail "the wizard did not open the second time"; fi

echo; [ $RC = 0 ] && echo "zip-check: PASS" || echo "zip-check: FAIL"
exit $RC
