#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# SG Store, as the Microsoft Store and winget do it:
#   A. a failed install says why: what the installer kind's own exit codes mean (NSIS 1:
#      cancelled; Inno Setup 5), and a package's own codes as its winget manifest's
#      ExpectedReturnCodes name them (installInProgress), the code beside
#   B. Update all: --update-all checks and updates every app with an update, one after another;
#      in the window, Check for updates shows "Update all (N)", and a click updates them
#   C. every category of the catalogue has its chip (Communication, Education, Security), the
#      chips wrapping onto a second row in a narrow window rather than being cut off
#   D. search results ranked: the name's beginning, a word of it, anywhere in it, then the
#      maker, then the description; sections in the order of their best answer
# Mutants (built here): NOEXITREASON, NOEXPECTED (A), NOUPDATEALL (B), FEWCATS, CHIPSCUT (C),
# NORANK (D), CHECKALL (B: Check for updates resolves apps that are not installed). Needs wine-sg (SG_WINE_DIR), Xvfb, xdotool and mingw; skips (77) without them.
# Run it in a private network (it serves its downloads on 127.0.0.1).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_STORE_EXE:-$HERE/build/sg-store64.exe}"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; XP=""; HP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in Xvfb xdotool python3 "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-store-ux.XXXXXX); chmod 755 "$T"
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
for m in NOEXITREASON NOEXPECTED NOUPDATEALL FEWCATS CHIPSCUT NORANK CHECKALL; do
    # shellcheck disable=SC2086
    "$MINGW" -municode -mwindows -O1 -Wno-missing-field-initializers -I"$HERE/src/browser" -I"$HERE/src/store" -I"$HERE/src/zip" \
        "-DSG_MUTANT_$m" -o "$T/mut-$(echo $m | tr 'A-Z' 'a-z').exe" $STORE_SRC $STORE_LIBS 2>>"$T/cc.log" \
        || fail "mutant $m does not build: $(tail -3 "$T/cc.log")"
done
"$MINGW" -municode -O1 -o "$T/fake.exe" "$HERE/test/sg-store-fake.c" -lole32 -luuid -lshell32 || { fail "the stand-in does not build"; exit 1; }

# --- the downloads, and winget-pkgs' layout ------------------------------------------------------
W="$T/www"; mkdir -p "$W/files" "$W/api/f/Fake" "$W/raw/f/Fake/Codes/1.0"
cp "$T/fake.exe" "$W/files/setup.exe"
SHA=$(sha256sum "$W/files/setup.exe" | cut -d' ' -f1)
mkzip() { python3 - "$1" "$T/fake.exe" "$2" <<'EOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    z.write(sys.argv[2], "tool/Tool.exe"); z.writestr("tool/version.txt", sys.argv[3])
EOF
}
mkzip "$W/files/tool-1.zip" 1; mkzip "$W/files/tool-2.zip" 2
Z1=$(sha256sum "$W/files/tool-1.zip" | cut -d' ' -f1); Z2=$(sha256sum "$W/files/tool-2.zip" | cut -d' ' -f1)
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
printf '[{"name": "1.0", "type": "dir"}]' > "$W/api/f/Fake/Codes"
cat > "$W/raw/f/Fake/Codes/1.0/Fake.Codes.installer.yaml" <<EOF
PackageIdentifier: Fake.Codes
PackageVersion: 1.0
InstallerType: exe
InstallerSwitches:
  Silent: /S
ExpectedReturnCodes:
- InstallerReturnCode: 77
  ReturnResponse: installInProgress
Installers:
- Architecture: x64
  InstallerUrl: http://127.0.0.1:$PORT/files/setup.exe
  InstallerSha256: $SHA
ManifestType: installer
EOF
(cd "$W" && exec python3 "$HERE/test/store-httpd.py" "$PORT" > "$T/http.log" 2>&1) & HP=$!

Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
DPY=$(cat "$T/display" 2>/dev/null); [ -n "$DPY" ] || { echo "FAIL  Xvfb did not start"; exit 1; }
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH" SG_STORE_DIRECT=1 SG_STORE_NOICONS=1 SG_STORE_INSTALL_WAIT=2000 SG_STORE_REGISTER_WAIT=1000
export SG_BROWSER_LIST_URL="http://127.0.0.1:$PORT/api/" SG_BROWSER_RAW_URL="http://127.0.0.1:$PORT/raw/"
export SG_BROWSER_LATEST_URL="http://127.0.0.1:$PORT/latest.tsv" SG_BROWSER_INDEX_URL="http://127.0.0.1:$PORT/index.tsv" SG_BROWSER_CDN_URL="http://127.0.0.1:$PORT/cdn/"
wine wineboot --init >/dev/null 2>&1; wineserver -w
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
K='HKLM\Software\Stained Glass\Store\Apps'
wine reg delete "$K" /f >/dev/null 2>&1
app() { # ord name category source [detect] [description] [publisher]
    reg "$K\\$1" /v Name /d "$2"; reg "$K\\$1" /v Publisher /d "${7:-The gate}"; reg "$K\\$1" /v Category /d "$3"
    reg "$K\\$1" /v Description /d "${6:-$2, a gate app.}"; reg "$K\\$1" /v Tier /d windows; reg "$K\\$1" /v Source /d "$4"
    [ -n "${5:-}" ] && reg "$K\\$1" /v DetectName /d "$5"
}
U="http://127.0.0.1:$PORT/files"
app A1 'Nsis App' Utilities "pin:$U/setup.exe|$SHA|nullsoft|/S" 'Nsis App'; reg "$K\\A1" /v PinVersion /d 1.0
app A2 'Inno App' Utilities "pin:$U/setup.exe|$SHA|inno|/VERYSILENT" 'Inno App'; reg "$K\\A2" /v PinVersion /d 1.0
app A3 'Codes App' Utilities 'winget:Fake.Codes' 'Codes App'
app B1 'Tool' Utilities "pin:$U/tool-1.zip|$Z1|portable-zip|tool\\Tool.exe" Tool; reg "$K\\B1" /v PinVersion /d 1.0
app B2 'Setup App' Utilities "pin:$U/setup.exe|$SHA|nullsoft|/S" 'Setup App'; reg "$K\\B2" /v PinVersion /d 1.0
app C1 'Chatter' Communication "pin:$U/setup.exe|$SHA|exe|/S"
app C2 'Teacher' Education "pin:$U/setup.exe|$SHA|exe|/S"
app C3 'Guard' Security "pin:$U/setup.exe|$SHA|exe|/S"
app D1 'Artist' Graphics "pin:$U/setup.exe|$SHA|exe|/S" '' 'Draw and paint pictures.'
app D2 'Zen Paint' Graphics "pin:$U/setup.exe|$SHA|exe|/S"
app D3 'Paintbox' Utilities "pin:$U/setup.exe|$SHA|exe|/S"
app D4 'Brushes' Media "pin:$U/setup.exe|$SHA|exe|/S" '' 'Brushes for artists.' 'Paint Works'
wineserver -w

G="$WINEPREFIX/drive_c/gate"; mkdir -p "$G"
export SG_STORE_DUMP='C:\gate\dump'
D="$G/dump"
run() { rm -f "$D" "$D.result"; timeout 300 wine "${SG_MUT:-$EXE}" "$@" >/dev/null 2>&1; }
appline() { grep -a "^app $1 " "${2:-$D}" 2>/dev/null | head -1 | tr -d '\r'; }
result() { grep -a "^result $1 " "$D.result" 2>/dev/null | head -1 | tr -d '\r'; }

# --- A. a failed install says why ----------------------------------------------------------------
SG_FAKE_EXIT=1 run --install A1
result A1 | grep -q 'fail 1 The installation was cancelled\. (The installer stopped with code 1\.)' \
    && pass "an NSIS installer's code 1 says the installation was cancelled" || fail "A1: $(result A1)"
SG_FAKE_EXIT=5 run --install A2
result A2 | grep -q 'cancelled.*code 5' && pass "Inno Setup's code 5 too" || fail "A2: $(result A2)"
SG_FAKE_EXIT=77 run --install A3
result A3 | grep -q 'Another installation is in progress.*code 77' \
    && pass "a package's own code says what its manifest's ExpectedReturnCodes call it (installInProgress)" || fail "A3: $(result A3)"
SG_FAKE_EXIT=1 SG_MUT="$T/mut-noexitreason.exe" run --install A1
result A1 | grep -q 'fail 1 The installer stopped with code 1\.$' && pass "MUTANT NOEXITREASON says only the code (gate catches it)" || fail "NOEXITREASON not detected: $(result A1)"
SG_FAKE_EXIT=77 SG_MUT="$T/mut-noexpected.exe" run --install A3
result A3 | grep -q 'in progress' && fail "NOEXPECTED not detected: $(result A3)" || pass "MUTANT NOEXPECTED loses the manifest's meaning (gate catches it)"

# --- B. Update all -------------------------------------------------------------------------------
SG_FAKE_KEY=SetupApp SG_FAKE_DISPLAY='Setup App' SG_FAKE_VERSION=1.0 SG_FAKE_DIR='Setup App' run --install B2
run --install B1
run --list
appline B1 | grep -q 'state=installed installed=1.0' && appline B2 | grep -q 'state=installed installed=1.0' \
    && pass "two apps installed at 1.0" || fail "B setup: $(appline B1) / $(appline B2)"
newer() {
    reg "$K\\B1" /v Source /d "pin:$U/tool-2.zip|$Z2|portable-zip|tool\\Tool.exe"; reg "$K\\B1" /v PinVersion /d 2.0
    reg "$K\\B2" /v PinVersion /d 2.0
}
older() {
    reg "$K\\B1" /v Source /d "pin:$U/tool-1.zip|$Z1|portable-zip|tool\\Tool.exe"; reg "$K\\B1" /v PinVersion /d 1.0
    reg "$K\\B2" /v PinVersion /d 1.0
    SG_FAKE_KEY=SetupApp SG_FAKE_DISPLAY='Setup App' SG_FAKE_VERSION=1.0 SG_FAKE_DIR='Setup App' run --install B2
    run --install B1
}
newer
SG_FAKE_KEY=SetupApp SG_FAKE_DISPLAY='Setup App' SG_FAKE_VERSION=2.0 SG_FAKE_DIR='Setup App' SG_MUT="$T/mut-noupdateall.exe" run --update-all
run --list
appline B1 | grep -q 'installed=1.0' && pass "MUTANT NOUPDATEALL updates nothing (gate catches it)" || fail "NOUPDATEALL not detected: $(appline B1)"
SG_FAKE_KEY=SetupApp SG_FAKE_DISPLAY='Setup App' SG_FAKE_VERSION=2.0 SG_FAKE_DIR='Setup App' run --update-all; ec=$?
[ "$ec" = 0 ] && grep -aq '^result B1 ok' "$D.result" && grep -aq '^result B2 ok' "$D.result" && grep -aq '^updated 2 failed 0' "$D.result" \
    && pass "--update-all updates both" || fail "update-all exit $ec: $(tr -d '\r' < "$D.result" 2>/dev/null)"
run --list
appline B1 | grep -q 'installed=2.0' && appline B2 | grep -q 'installed=2.0' && ! grep -aq '^result [AC-D]' "$D.result" 2>/dev/null \
    && pass "both are 2.0 now, nothing else was touched" || fail "after update-all: $(appline B1) / $(appline B2)"
# the window: Check for updates, then Update all (2)
older; newer
click() { xy=$(grep -a "^hit $1 " "$D" 2>/dev/null | tail -1 | awk '{ print $(NF-1), $NF }'); [ -n "$xy" ] || return 1
          xdotool mousemove "${xy% *}" "${xy#* }" click 1; }
waitfor() { i=0; while ! grep -aq "$1" "$D" 2>/dev/null && [ $i -lt "${2:-120}" ]; do sleep 0.5; i=$((i + 1)); done; grep -aq "$1" "$D" 2>/dev/null; }
window() { # exe: the window, Check for updates, Update all; the dump stays in $D
    rm -f "$D"
    SG_FAKE_KEY=SetupApp SG_FAKE_DISPLAY='Setup App' SG_FAKE_VERSION=2.0 SG_FAKE_DIR='Setup App' wine "$1" >/dev/null 2>&1 & WP=$!
    waitfor '^hit checkall' 60 && click checkall && waitfor '^hit updateall' 60 && click updateall
    i=0; while ! { appline B1 | grep -q 'installed=2.0' && appline B2 | grep -q 'installed=2.0'; } && [ $i -lt 120 ]; do sleep 0.5; i=$((i + 1)); done
    kill "$WP" 2>/dev/null; wineserver -k; wineserver -w
}
window "$EXE"
appline B1 | grep -q 'installed=2.0' && appline B2 | grep -q 'installed=2.0' \
    && pass "the window's Update all updates both after Check for updates" || fail "window update all: $(grep -a '^hit updateall' "$D"); $(appline B1) / $(appline B2)"
appline A3 | grep -q 'available=- ' && grep -aq '^checking 0' "$D" \
    && pass "Check for updates looks only at installed apps (winget upgrade's way), on its own thread" || fail "not-installed A3 checked: $(appline A3); $(grep -a '^checking' "$D")"
older; newer
window "$T/mut-checkall.exe"
appline A3 | grep -q 'available=1.0 ' && pass "MUTANT CHECKALL resolves every app (gate catches it)" || fail "CHECKALL not detected: $(appline A3)"
older; newer
window "$T/mut-noupdateall.exe"
grep -aq '^hit updateall' "$D" && fail "NOUPDATEALL not detected in the window" || pass "MUTANT NOUPDATEALL shows no Update all (gate catches it)"
older

# --- C. every category has its chip ------------------------------------------------------------
chips() { # exe: the window's chips into $T/chips
    rm -f "$D"; wine "$1" >/dev/null 2>&1 & WP=$!
    waitfor '^hit category' 60; sleep 1
    grep -a '^hit category' "$D" | awk '{ print $3 }' > "$T/chips"
    kill "$WP" 2>/dev/null; wineserver -k; wineserver -w
}
chips "$EXE"
miss=""; for c in All Browsers Productivity Graphics Media Development Utilities Internet Games Communication Education Security Linux; do
    grep -qx "$c" "$T/chips" || miss="$miss $c"; done
[ -z "$miss" ] && pass "all thirteen category chips are shown" || fail "chips missing:$miss"
chips "$T/mut-fewcats.exe"; grep -qx Communication "$T/chips" && fail "FEWCATS not detected" || pass "MUTANT FEWCATS has no Communication chip (gate catches it)"
chips "$T/mut-chipscut.exe"; grep -qx Linux "$T/chips" && fail "CHIPSCUT not detected" || pass "MUTANT CHIPSCUT cuts the last chips off (gate catches it)"

# --- D. search ranking -------------------------------------------------------------------------
view() { # exe query: the list's order
    rm -f "$D"; wine "$1" --search "$2" >/dev/null 2>&1 & WP=$!
    waitfor '^view' 60; sleep 1
    grep -a '^view' "$D" | tail -1 | cut -d' ' -f3- | tr -d '\r'
    kill "$WP" 2>/dev/null; wineserver -k; wineserver -w
}
v=$(view "$EXE" paint)
[ "$v" = "D3 D2 D1 D4" ] && pass "\"paint\": Paintbox (the name's beginning) first, then Zen Paint (a word) before Artist (its description), then Brushes (the maker)" \
    || fail "paint ranked: $v"
v=$(view "$T/mut-norank.exe" paint)
[ "$v" != "D3 D2 D1 D4" ] && pass "MUTANT NORANK lists by category and name (gate catches it): $v" || fail "NORANK not detected"
wineserver -w

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
