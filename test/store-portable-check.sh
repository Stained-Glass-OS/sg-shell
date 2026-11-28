#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# SG Store's portable apps from a pinned download (pin: Type portable-zip and
# portable), as winget installs its zip + portable packages:
#   A. a zip of the program's files is put whole in the person's
#      %LOCALAPPDATA%\Microsoft\WinGet\Packages\<name>__SgStore, the program the
#      entry's Silent field names; an Uninstall entry of its own (version,
#      program as its icon, size, date, the files it put there); a Start
#      shortcut to the program; the version detected
#   B. a newer PinVersion is an update; the update unpacks the new version,
#      takes away the old version's files the new one does not have and keeps
#      what the program made there itself
#   C. Uninstall takes the folder, the entry and the shortcut away
#   D. a bare program (Type portable) is put in its folder under its own name
#   E. Scope machine: an elevated copy of the store puts it in
#      %ProgramFiles%\WinGet\Packages, with the machine's entry and a shortcut in
#      everyone's Start; the elevated copy checks the download itself (a changed
#      file is refused); Uninstall takes it all away
# Mutants (built here):
#   SG_MUTANT_NOPINPORTABLE    a pinned portable zip is run as an installer (ILSpy: nothing installed)
#   SG_MUTANT_PORTABLEKEEPOLD  an update leaves the old version's files behind
#   SG_MUTANT_PORTABLEUSERONLY Scope machine is ignored: the person's folder
# Needs wine-sg (SG_WINE_DIR), Xvfb and mingw; skips (77) without them.
# Run it in a private network (it serves its downloads on 127.0.0.1).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_STORE_EXE:-$HERE/build/sg-store64.exe}"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; XP=""; HP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in Xvfb python3 "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-store-portable.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    sg_kill_prefix "$T/pfx"
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    [ -n "$HP" ] && kill "$HP" 2>/dev/null
    [ -n "${SG_KEEP:-}" ] || rm -rf "$T"
}
trap cleanup EXIT INT TERM

STORE_SRC="$HERE/src/store/main.c $HERE/src/store/details.c $HERE/src/store/catalog.c $HERE/src/store/sysinstall.c $HERE/src/store/icons.c $HERE/src/store/msix.c $HERE/src/browser/fetch.c $HERE/src/browser/manifest.c $HERE/src/zip/zipcore.c"
STORE_LIBS="-lruntimeobject -lsetupapi -lwininet -lbcrypt -lshlwapi -lshell32 -lgdi32 -luser32 -ladvapi32 -lole32 -luuid -lwindowscodecs -lmsimg32 -lcomdlg32"
for m in NOPINPORTABLE PORTABLEKEEPOLD PORTABLEUSERONLY STALELINK ENTRYFIRST; do
    # shellcheck disable=SC2086
    "$MINGW" -municode -mwindows -O1 -Wno-missing-field-initializers -I"$HERE/src/browser" -I"$HERE/src/store" -I"$HERE/src/zip" \
        "-DSG_MUTANT_$m" -o "$T/mut-$(echo $m | tr 'A-Z' 'a-z').exe" $STORE_SRC $STORE_LIBS 2>>"$T/cc.log" \
        || fail "mutant $m does not build: $(tail -3 "$T/cc.log")"
done
"$MINGW" -municode -O1 -o "$T/viewer.exe" "$HERE/test/sg-store-fake.c" -lole32 -luuid -lshell32 || { fail "the stand-in does not build"; exit 1; }

# --- the downloads -------------------------------------------------------------------------------
W="$T/www"; mkdir -p "$W/files"
mkzip() { # out: version 1 has data/old.txt, version 2 data/new.txt
    python3 - "$1" "$T/viewer.exe" "$2" <<'EOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    z.write(sys.argv[2], "Viewer-%s/bin/Viewer.exe" % "x")
    z.writestr("Viewer-x/readme.txt", "version " + sys.argv[3])
    z.writestr("Viewer-x/data/%s.txt" % ("old" if sys.argv[3] == "1" else "new"), "data " + sys.argv[3])
    z.writestr("Viewer-x/plugins/v%s/p.dll" % sys.argv[3], "plugin")
EOF
}
mkzip "$W/files/viewer-1.zip" 1; mkzip "$W/files/viewer-2.zip" 2
cp "$T/viewer.exe" "$W/files/baretool.exe"; cp "$T/viewer.exe" "$W/files/baretool-2.1.exe"
S1=$(sha256sum "$W/files/viewer-1.zip" | cut -d' ' -f1); S2=$(sha256sum "$W/files/viewer-2.zip" | cut -d' ' -f1)
SB=$(sha256sum "$W/files/baretool.exe" | cut -d' ' -f1)
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
(cd "$W" && exec python3 "$HERE/test/store-httpd.py" "$PORT" > "$T/http.log" 2>&1) & HP=$!

Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
DPY=$(cat "$T/display" 2>/dev/null); [ -n "$DPY" ] || { echo "FAIL  Xvfb did not start"; exit 1; }
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH" SG_STORE_DIRECT=1
wine wineboot --init >/dev/null 2>&1; wineserver -w
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
K='HKLM\Software\Stained Glass\Store\Apps'
wine reg delete "$K" /f >/dev/null 2>&1
app() { # ord name source version
    reg "$K\\$1" /v Name /d "$2"; reg "$K\\$1" /v Publisher /d 'The gate'; reg "$K\\$1" /v Category /d Utilities
    reg "$K\\$1" /v Tier /d windows; reg "$K\\$1" /v Source /d "$3"; reg "$K\\$1" /v PinVersion /d "$4"
}
U="http://127.0.0.1:$PORT/files"
app P1 'Zip Viewer' "pin:$U/viewer-1.zip|$S1|portable-zip|Viewer-x\\bin\\Viewer.exe" 1.0
app P2 'Bare Tool' "pin:$U/baretool.exe|$SB|portable|" 2.0
app P3 'Machine Viewer' "pin:$U/viewer-1.zip|$S1|portable-zip|Viewer.exe" 1.0
reg "$K\\P3" /v Scope /d machine
wineserver -w

G="$WINEPREFIX/drive_c/gate"; mkdir -p "$G"
export SG_STORE_DUMP='C:\gate\dump'
D="$G/dump"
run() { rm -f "$D" "$D.result"; wine "${SG_MUT:-$EXE}" "$@" >/dev/null 2>&1; }
appline() { grep -a "^app $1 " "$D" 2>/dev/null | head -1 | tr -d '\r'; }
UNK='Software\Microsoft\Windows\CurrentVersion\Uninstall'
q() { wine reg query "$1" /v "$2" 2>/dev/null | tr -d '\r' | awk -v v="$2" '$1 == v { $1 = ""; $2 = ""; sub(/^ +/, ""); print }'; }
PROF=$(find "$WINEPREFIX/drive_c/users" -maxdepth 1 -mindepth 1 -type d ! -name Public | head -1)
LOCAL="$PROF/AppData/Local/Microsoft/WinGet/Packages"
PROGS="$PROF/AppData/Roaming/Microsoft/Windows/Start Menu/Programs"
CPROGS="$WINEPREFIX/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs"
PF="$WINEPREFIX/drive_c/Program Files/WinGet/Packages"

# --- A. a portable zip, the person's -----------------------------------------------------------
P="$LOCAL/Zip Viewer__SgStore"
run --install P1; ec=$?
[ "$ec" = 0 ] && [ -f "$P/Viewer-x/bin/Viewer.exe" ] && [ -f "$P/Viewer-x/data/old.txt" ] && [ -f "$P/Viewer-x/readme.txt" ] \
    && pass "a pinned portable zip is put whole in the person's WinGet Packages folder" \
    || fail "install P1 exit $ec: $(cat "$D.result" 2>/dev/null); $(ls -R "$LOCAL" 2>&1 | head -5)"
E="HKCU\\$UNK\\Zip Viewer__SgStore"
[ "$(q "$E" DisplayVersion)" = 1.0 ] && q "$E" DisplayIcon | grep -q 'Viewer-x.bin.Viewer\.exe$' \
    && pass "its Uninstall entry has the pinned version and the program the entry names as its icon" \
    || fail "entry: $(wine reg query "$E" 2>&1 | tr -d '\r' | head -12)"
[ -n "$(q "$E" InstallDate)" ] && [ "$(q "$E" EstimatedSize)" != "" ] && q "$E" SgStoreFiles | grep -q 'old\.txt' \
    && pass "and its install date, size and the files it put there" || fail "entry date/size/files: $(wine reg query "$E" 2>&1 | tr -d '\r' | head -14)"
[ -f "$PROGS/Zip Viewer.lnk" ] && pass "a Start shortcut to it" || fail "no Start shortcut: $(ls "$PROGS" 2>&1)"
run --list
appline P1 | grep -q 'state=installed installed=1.0' && pass "it is detected, version 1.0" || fail "P1 after install: $(appline P1)"

# --- B. the update -----------------------------------------------------------------------------
echo mine > "$P/Viewer-x/data/user.txt"
reg "$K\\P1" /v Source /d "pin:$U/viewer-2.zip|$S2|portable-zip|Viewer-x\\bin\\Viewer.exe"; reg "$K\\P1" /v PinVersion /d 2.0
run --check-updates
appline P1 | grep -q 'state=update installed=1.0 available=2.0' && pass "a newer PinVersion is an update" || fail "P1 check: $(appline P1)"
run --install P1; ec=$?
[ "$ec" = 0 ] && [ "$(q "$E" DisplayVersion)" = 2.0 ] && [ -f "$P/Viewer-x/data/new.txt" ] && grep -q 'version 2' "$P/Viewer-x/readme.txt" \
    && pass "the update unpacks version 2.0" || fail "update exit $ec: $(cat "$D.result" 2>/dev/null)"
[ ! -e "$P/Viewer-x/data/old.txt" ] && [ ! -e "$P/Viewer-x/plugins/v1" ] && [ -f "$P/Viewer-x/plugins/v2/p.dll" ] \
    && pass "and takes away what only the old version had (and its emptied folder)" || fail "old files left: $(ls -R "$P" | head -20)"
[ -f "$P/Viewer-x/data/user.txt" ] && pass "what the program made there itself stays" || fail "the program's own file was deleted"
[ "$(find "$PROGS" -name '*.lnk' | wc -l)" = 1 ] && pass "still one Start shortcut" || fail "shortcuts: $(ls "$PROGS")"

# --- C. Uninstall --------------------------------------------------------------------------------
# a folder that cannot be taken away (a program still running from it): the entry stays, the store
# says so, and the app can be uninstalled again later
chmod 555 "$P/Viewer-x/data"
SG_STORE_UNINSTALL_GRACE=2000 run --uninstall P1
grep -q '^uninstall P1 fail' "$D.result" 2>/dev/null && wine reg query "$E" >/dev/null 2>&1 && [ -f "$P/Viewer-x/data/new.txt" ] \
    && pass "a folder that cannot be removed keeps its entry, and the store says it was not removed" \
    || fail "busy folder: $(cat "$D.result" 2>/dev/null); entry $(wine reg query "$E" >/dev/null 2>&1 && echo kept || echo gone)"
chmod 755 "$P/Viewer-x/data"
SG_MUT="$T/mut-entryfirst.exe" run --install P1
chmod 555 "$P/Viewer-x/data"
SG_STORE_UNINSTALL_GRACE=2000 run --uninstall P1
wine reg query "$E" >/dev/null 2>&1 && fail "ENTRYFIRST not detected" || pass "MUTANT ENTRYFIRST takes the entry and leaves the folder (gate catches it)"
chmod 755 "$P/Viewer-x/data"
run --install P1
run --uninstall P1
grep -q '^uninstall P1 ok' "$D.result" 2>/dev/null && [ ! -e "$P/Viewer-x/bin" ] && [ ! -e "$P/Viewer-x/readme.txt" ] && [ ! -e "$P/Viewer-x/plugins" ] \
    && ! wine reg query "$E" >/dev/null 2>&1 && [ ! -e "$PROGS/Zip Viewer.lnk" ] \
    && pass "Uninstall takes its files, entry and Start shortcut away" || fail "uninstall P1: $(cat "$D.result" 2>/dev/null); $(ls -R "$P" "$PROGS" 2>&1 | head -8)"
[ "$(cd "$P" 2>/dev/null && find . -type f)" = "./Viewer-x/data/user.txt" ] && pass "and keeps what the program made there (winget's default)" \
    || fail "after uninstall: $(cd "$P" 2>/dev/null && find .)"
rm -rf "$P"

# the mutants of A and B
reg "$K\\P1" /v Source /d "pin:$U/viewer-1.zip|$S1|portable-zip|Viewer-x\\bin\\Viewer.exe"; reg "$K\\P1" /v PinVersion /d 1.0
SG_MUT="$T/mut-nopinportable.exe" run --install P1
[ ! -f "$P/Viewer-x/bin/Viewer.exe" ] && pass "MUTANT NOPINPORTABLE installs nothing (gate catches it)" || fail "NOPINPORTABLE not detected"
run --uninstall P1
run --install P1
reg "$K\\P1" /v Source /d "pin:$U/viewer-2.zip|$S2|portable-zip|Viewer-x\\bin\\Viewer.exe"; reg "$K\\P1" /v PinVersion /d 2.0
SG_MUT="$T/mut-portablekeepold.exe" run --install P1
[ -e "$P/Viewer-x/data/old.txt" ] && pass "MUTANT PORTABLEKEEPOLD leaves the old files (gate catches it)" || fail "PORTABLEKEEPOLD not detected"
run --uninstall P1

# --- D. a bare program ---------------------------------------------------------------------------
run --install P2; ec=$?
[ "$ec" = 0 ] && [ -f "$LOCAL/Bare Tool__SgStore/baretool.exe" ] && [ "$(q "HKCU\\$UNK\\Bare Tool__SgStore" DisplayVersion)" = 2.0 ] \
    && pass "a pinned bare program is put in its own folder under its own name" || fail "install P2 exit $ec: $(cat "$D.result" 2>/dev/null)"
# its update named by its version (Rufus's rufus-4.15.exe, then rufus-4.16.exe): the old program goes, and the
# Start shortcut follows the new one
lnkto() { python3 - "$1" <<'EOF2'
import sys, re
d = open(sys.argv[1], 'rb').read()
m = re.findall(rb'[A-Za-z]:\\[\x20-\x7e]+?\.exe', d)
print(m[0].decode() if m else '')
EOF2
}
reg "$K\\P2" /v Source /d "pin:$U/baretool-2.1.exe|$SB|portable|"; reg "$K\\P2" /v PinVersion /d 2.1
run --install P2; ec=$?
[ "$ec" = 0 ] && [ -f "$LOCAL/Bare Tool__SgStore/baretool-2.1.exe" ] && [ ! -e "$LOCAL/Bare Tool__SgStore/baretool.exe" ] \
    && pass "its update under a new name takes the old program away" || fail "P2 update exit $ec: $(ls "$LOCAL/Bare Tool__SgStore" 2>&1)"
lnkto "$PROGS/Bare Tool.lnk" | grep -q 'baretool-2\.1\.exe$' && pass "and its Start shortcut goes to the new one" || fail "P2 shortcut goes to '$(lnkto "$PROGS/Bare Tool.lnk")'"
run --uninstall P2
[ ! -d "$LOCAL/Bare Tool__SgStore" ] && [ ! -e "$PROGS/Bare Tool.lnk" ] && pass "and uninstalled" || fail "P2 left: $(cat "$D.result" 2>/dev/null)"
reg "$K\\P2" /v Source /d "pin:$U/baretool.exe|$SB|portable|"; reg "$K\\P2" /v PinVersion /d 2.0
run --install P2
reg "$K\\P2" /v Source /d "pin:$U/baretool-2.1.exe|$SB|portable|"; reg "$K\\P2" /v PinVersion /d 2.1
SG_MUT="$T/mut-stalelink.exe" run --install P2
lnkto "$PROGS/Bare Tool.lnk" | grep -q 'baretool\.exe$' && pass "MUTANT STALELINK keeps the shortcut to the old program (gate catches it)" || fail "STALELINK not detected: '$(lnkto "$PROGS/Bare Tool.lnk")'"
run --uninstall P2

# --- E. for the machine --------------------------------------------------------------------------
M="$PF/Machine Viewer__SgStore"; ME="HKLM\\$UNK\\Machine Viewer__SgStore"
run --install P3; ec=$?
[ "$ec" = 0 ] && [ -f "$M/Viewer-x/bin/Viewer.exe" ] && [ ! -d "$LOCAL/Machine Viewer__SgStore" ] \
    && pass "Scope machine: it is put in Program Files' WinGet Packages" || fail "install P3 exit $ec: $(cat "$D.result" 2>/dev/null); $(ls "$PF" 2>&1)"
grep -q '^elevated P3 1' "$D.result" 2>/dev/null && pass "by the store's elevated copy" || fail "P3 not elevated: $(cat "$D.result" 2>/dev/null)"
[ "$(q "$ME" DisplayVersion)" = 1.0 ] && q "$ME" UninstallString | grep -q 'HKLM' && ! wine reg query "HKCU\\$UNK\\Machine Viewer__SgStore" >/dev/null 2>&1 \
    && pass "with the machine's Uninstall entry" || fail "machine entry: $(wine reg query "$ME" 2>&1 | tr -d '\r' | head -10)"
[ -f "$CPROGS/Machine Viewer.lnk" ] && [ ! -e "$PROGS/Machine Viewer.lnk" ] && pass "and its shortcut in everyone's Start (none of the person's)" \
    || fail "machine shortcut: $(ls "$CPROGS" "$PROGS" 2>&1)"
run --uninstall P3
grep -q '^uninstall P3 ok' "$D.result" 2>/dev/null && [ ! -d "$M" ] && ! wine reg query "$ME" >/dev/null 2>&1 && [ ! -e "$CPROGS/Machine Viewer.lnk" ] \
    && pass "Uninstall takes the machine's folder, entry and shortcut away" || fail "uninstall P3: $(cat "$D.result" 2>/dev/null); $(ls "$M" "$CPROGS" 2>&1 | head)"
# the elevated copy checks the file it is handed itself
cp "$W/files/viewer-2.zip" "$G/swapped.zip"
timeout 120 wine "$EXE" --elevated-portable P3 'C:\gate\swapped.zip' >/dev/null 2>&1; ec=$?
[ "$ec" != 0 ] && [ ! -d "$M" ] && pass "the elevated copy refuses a download that is not the pinned one" || fail "swapped file installed (exit $ec)"
timeout 120 wine "$EXE" --elevated-portable P1 'C:\gate\swapped.zip' >/dev/null 2>&1; ec=$?
[ "$ec" != 0 ] && [ ! -d "$PF/Zip Viewer__SgStore" ] && pass "and an app the catalogue does not put in Program Files" || fail "--elevated-portable of a person's app (exit $ec)"
SG_MUT="$T/mut-portableuseronly.exe" run --install P3
[ ! -d "$M" ] && [ -d "$LOCAL/Machine Viewer__SgStore" ] && pass "MUTANT PORTABLEUSERONLY puts it in the person's folder (gate catches it)" || fail "PORTABLEUSERONLY not detected"
run --uninstall P3
wineserver -w

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
