#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for SG Store (sg-store64.exe): the catalogue engine and the window.
#
# Against a winget-pkgs-shaped source served from this machine (python3 -m
# http.server), a stand-in installer (test/sg-store-fake.c) and a gate
# catalogue in HKLM\Software\Stained Glass\Store, headlessly:
#
#   - --list loads the catalogue; the Windows/our tiers are shown by default
#     and the Linux tier is hidden (shown=0)
#   - --check-updates marks an installed app with an older version as an update
#     and names the newer available version
#   - --install of an app whose download is not the file its manifest's SHA-256
#     names is refused and the installer never runs
#   - --install through a winget manifest downloads, checks the SHA-256 and runs
#     the installer; the app is then detected as installed with its version
#   - --install of a pinned entry downloads, checks the catalogue's SHA-256 and
#     installs
#   - an "ours" app whose setup program comes in a system package (SG Office:
#     sg-office) that this machine does not have -- a machine made before the
#     package was in the image -- is installed anyway: the store elevates, the
#     elevated copy has sg-admind (run here in its test mode, with stand-ins
#     for apt-get and runuser) install the package and register it, then runs
#     the setup program; a package not on sg-admind's list is refused
#   - the window opens, hides the Linux tier, and the Advanced disclosure shows
#     it
#
# Mutants (built here from source): -DSG_MUTANT_NOHASH installs an unverified
# download; -DSG_MUTANT_SHOWLINUX shows the Linux tier by default;
# -DSG_MUTANT_NOUPDATE never notices a newer version; -DSG_MUTANT_NOPACKAGE
# never has the missing package installed -- each must turn it red.
#
# Needs wine-sg, mingw, gcc, Xvfb, xdotool, ImageMagick, python3; skips (77)
# without them. SG_STORE_EXE tests another build.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_STORE_EXE:-$HERE/build/sg-store64.exe}"
OUT="$HERE/build"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; XP=""; HP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import python3 "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-store-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    [ -n "$HP" ] && kill "$HP" 2>/dev/null
    rm -f "$T/admind-run"; [ -n "${AP:-}" ] && kill "$AP" 2>/dev/null
    [ -n "${SG_KEEP:-}" ] || rm -rf "$T"
}
trap cleanup EXIT INT TERM

STORE_SRC="$HERE/src/store/main.c $HERE/src/store/catalog.c $HERE/src/browser/fetch.c $HERE/src/browser/manifest.c"
STORE_LIBS="-lwininet -lbcrypt -lshlwapi -lshell32 -lgdi32 -luser32 -ladvapi32 -lole32 -lmsimg32"
build_mut() { # define outfile
    # shellcheck disable=SC2086
    "$MINGW" -municode -mwindows -O1 -Wno-missing-field-initializers -I"$HERE/src/browser" -I"$HERE/src/store" \
        "-D$1" -o "$2" $STORE_SRC $STORE_LIBS 2>>"$T/cc.log"
}
build_mut SG_MUTANT_NOHASH   "$T/mut-nohash.exe"   || { fail "mutant NOHASH does not build: $(tail -3 "$T/cc.log")"; }
build_mut SG_MUTANT_SHOWLINUX "$T/mut-showlinux.exe" || fail "mutant SHOWLINUX does not build"
build_mut SG_MUTANT_NOUPDATE "$T/mut-noupdate.exe"  || fail "mutant NOUPDATE does not build"
build_mut SG_MUTANT_NOPACKAGE "$T/mut-nopackage.exe" || fail "mutant NOPACKAGE does not build"

# the stand-ins
"$MINGW" -municode -O1 -o "$T/store-fake.exe" "$HERE/test/sg-store-fake.c" || { fail "the stand-ins do not build"; exit 1; }
mkdir -p "$T/wg"; cp "$T/store-fake.exe" "$T/wg/winget.exe"

# --- the source: winget-pkgs' layout on this machine ---------------------------------------------
W="$T/www"; mkdir -p "$W/api/f/Fake" "$W/api/b/Bad" "$W/raw/f/Fake/App/1.10.0" "$W/raw/b/Bad/Hash/2.0" "$W/files"
cp "$T/store-fake.exe" "$W/files/store-fake.exe"
SHA=$(sha256sum "$W/files/store-fake.exe" | cut -d' ' -f1)
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
printf '[{"name": "1.9.0", "type": "dir"}, {"name": "1.10.0", "type": "dir"}]' > "$W/api/f/Fake/App"
printf '[{"name": "2.0", "type": "dir"}]' > "$W/api/b/Bad/Hash"
cat > "$W/raw/f/Fake/App/1.10.0/Fake.App.installer.yaml" <<EOF
PackageIdentifier: Fake.App
PackageVersion: 1.10.0
InstallerType: nullsoft
Scope: machine
InstallerSwitches:
  Silent: /S /gate-silent
Installers:
- Architecture: x86
  InstallerUrl: http://127.0.0.1:$PORT/files/wrong-arch.exe
  InstallerSha256: 0000000000000000000000000000000000000000000000000000000000000000
- Architecture: x64
  InstallerUrl: http://127.0.0.1:$PORT/files/store-fake.exe
  InstallerSha256: $SHA
ManifestType: installer
EOF
cat > "$W/raw/b/Bad/Hash/2.0/Bad.Hash.installer.yaml" <<EOF
PackageIdentifier: Bad.Hash
PackageVersion: 2.0
InstallerType: nullsoft
Installers:
- Architecture: x64
  InstallerUrl: http://127.0.0.1:$PORT/files/store-fake.exe
  InstallerSha256: 1111111111111111111111111111111111111111111111111111111111111111
EOF
(cd "$W" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 > "$T/http.log" 2>&1) & HP=$!

Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
DPY=$(cat "$T/display" 2>/dev/null); [ -n "$DPY" ] || { echo "FAIL  Xvfb did not start"; exit 1; }
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
export SG_BROWSER_LIST_URL="http://127.0.0.1:$PORT/api/" SG_BROWSER_RAW_URL="http://127.0.0.1:$PORT/raw/"
wine wineboot --init >/dev/null 2>&1; wineserver -w
reg() { wine reg add "$@" /f >/dev/null 2>&1; }

# --- the gate's catalogue ------------------------------------------------------------------------
K='HKLM\Software\Stained Glass\Store\Apps'
wine reg delete "$K" /f >/dev/null 2>&1
reg "$K\\01" /v Name /d 'Fake App'
reg "$K\\01" /v Publisher /d 'The gate'
reg "$K\\01" /v Description /d 'Installs from this machine through a winget manifest.'
reg "$K\\01" /v Category /d 'Browsers'
reg "$K\\01" /v Tier /d windows
reg "$K\\01" /v Source /d 'winget:Fake.App'
reg "$K\\01" /v DetectName /d 'Fake App'
reg "$K\\02" /v Name /d 'Pinned App'
reg "$K\\02" /v Publisher /d 'The gate'
reg "$K\\02" /v Description /d 'A pinned vendor download.'
reg "$K\\02" /v Category /d 'Utilities'
reg "$K\\02" /v Tier /d windows
reg "$K\\02" /v Source /d "pin:http://127.0.0.1:$PORT/files/store-fake.exe|$SHA|nullsoft|/S"
reg "$K\\02" /v PinVersion /d '3.0'
reg "$K\\02" /v DetectName /d 'Pinned App'
reg "$K\\04" /v Name /d 'Tampered App'
reg "$K\\04" /v Publisher /d 'Nobody'
reg "$K\\04" /v Description /d 'Its download does not match its manifest.'
reg "$K\\04" /v Category /d 'Utilities'
reg "$K\\04" /v Tier /d windows
reg "$K\\04" /v Source /d 'winget:Bad.Hash'
reg "$K\\04" /v DetectName /d 'Bad'
reg "$K\\90" /v Name /d 'GIMP (Linux)'
reg "$K\\90" /v Publisher /d 'The GIMP Team'
reg "$K\\90" /v Description /d 'A native Linux app -- a last resort.'
reg "$K\\90" /v Category /d 'Graphics'
reg "$K\\90" /v Tier /d linux
reg "$K\\90" /v Source /d 'linux:apt:gimp'
reg "$K\\90" /v DetectName /d 'GIMP'
# App Paths for the built store (so --install can find setup programs if any)
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\sg-store.exe' /ve /d "$winexe"
wineserver -w

G="$WINEPREFIX/drive_c/gate"; mkdir -p "$G"
export SG_STORE_DUMP='C:\gate\dump'
D="$G/dump"
field() { sed -n "s/^$1 //p" "${2:-$D}" 2>/dev/null | head -1; }
appline() { grep "^app $1 " "$D" 2>/dev/null | head -1; }
kv() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }
run() { rm -f "$D" "$D.result"; wine "${SG_MUT:-$EXE}" "$@" >/dev/null 2>&1; }

# --- A. --list: tiers, and the Linux tier hidden -------------------------------------------------
run --list
[ "$(field catalog)" = 4 ] && pass "the catalogue loads (4 apps)" || fail "catalog '$(field catalog)'"
a01=$(appline 01); a90=$(appline 90)
[ "$(kv "$a01" shown)" = 1 ] && pass "a Windows-tier app is shown by default" || fail "01 shown '$(kv "$a01" shown)'"
[ "$(kv "$a90" shown)" = 0 ] && pass "the Linux-tier app is hidden by default" || fail "90 shown '$(kv "$a90" shown)'"
[ "$(kv "$a90" tier)" = linux ] && pass "the Linux app's tier is 'linux'" || fail "90 tier '$(kv "$a90" tier)'"
# mutant: the Linux tier leaks into the default view
SG_MUT="$T/mut-showlinux.exe" run --list
[ "$(kv "$(appline 90)" shown)" = 1 ] && pass "MUTANT SHOWLINUX would show the Linux tier (gate catches it)" || fail "SHOWLINUX not detected by the gate"

# --- B. update detection --------------------------------------------------------------------------
UN='HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\FakeApp'
reg "$UN" /v DisplayName /d 'Fake App'
reg "$UN" /v DisplayVersion /d '1.0.0'
wineserver -w
run --check-updates
a01=$(appline 01)
[ "$(kv "$a01" installed)" = 1.0.0 ] && pass "an installed app's version is detected (1.0.0)" || fail "01 installed '$(kv "$a01" installed)'"
[ "$(kv "$a01" available)" = 1.10.0 ] && pass "the newer available version is resolved (1.10.0)" || fail "01 available '$(kv "$a01" available)'"
[ "$(kv "$a01" state)" = update ] && pass "the app is marked as an update" || fail "01 state '$(kv "$a01" state)'"
SG_MUT="$T/mut-noupdate.exe" run --check-updates
[ "$(kv "$(appline 01)" state)" != update ] && pass "MUTANT NOUPDATE misses the update (gate catches it)" || fail "NOUPDATE not detected by the gate"
wine reg delete "$UN" /f >/dev/null 2>&1; wineserver -w

# --- C. a tampered download is refused ------------------------------------------------------------
rm -f "$G/setup.log"
run --install 04
[ ! -e "$G/setup.log" ] && pass "a download whose SHA-256 is not the manifest's is refused, the installer never runs" \
    || fail "the tampered installer ran: $(cat "$G/setup.log" 2>/dev/null)"
grep -qi 'SHA-256' "$D.result" 2>/dev/null && pass "and the reason names the SHA-256 mismatch" || fail "result '$(cat "$D.result" 2>/dev/null)'"
# mutant: no hash check -> the tampered installer runs
rm -f "$G/setup.log"
SG_MUT="$T/mut-nohash.exe" run --install 04
[ -e "$G/setup.log" ] && pass "MUTANT NOHASH runs the unverified installer (gate catches it)" || fail "NOHASH not detected by the gate"
wine reg delete 'HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\FakeApp' /f >/dev/null 2>&1
rm -f "$G/setup.log"; wineserver -w

# --- D. a winget manifest install -----------------------------------------------------------------
SG_FAKE_KEY=FakeApp SG_FAKE_DISPLAY='Fake App' SG_FAKE_VERSION=1.10.0 SG_FAKE_DIR='Fake App' \
    wine "$EXE" --install 01 >/dev/null 2>&1; ec=$?
[ "$ec" = 0 ] && pass "--install through the manifest succeeds" || fail "install 01 exit $ec: $(cat "$D.result" 2>/dev/null)"
grep -q '/S /gate-silent' "$G/setup.log" 2>/dev/null && pass "run with the manifest's silent switches" || fail "setup.log '$(cat "$G/setup.log" 2>/dev/null)'"
[ -f "$WINEPREFIX/drive_c/Program Files/Fake App/app.exe" ] && pass "the app is installed" || fail "app.exe not installed"
run --list
[ "$(kv "$(appline 01)" state)" = installed ] && [ "$(kv "$(appline 01)" installed)" = 1.10.0 ] \
    && pass "and is then detected as installed (1.10.0)" || fail "after install: $(appline 01)"

# --- E. a pinned install --------------------------------------------------------------------------
rm -f "$G/setup.log"
SG_FAKE_KEY=PinnedApp SG_FAKE_DISPLAY='Pinned App' SG_FAKE_VERSION=3.0 SG_FAKE_DIR='Pinned App' \
    wine "$EXE" --install 02 >/dev/null 2>&1; ec=$?
[ "$ec" = 0 ] && pass "a pinned entry installs (SHA-256 checked from the catalogue)" || fail "install 02 exit $ec: $(cat "$D.result" 2>/dev/null)"
[ -f "$WINEPREFIX/drive_c/Program Files/Pinned App/app.exe" ] && pass "the pinned app is installed" || fail "pinned app.exe not installed"

# --- E2. an "ours" app whose setup program is in a system package this machine lacks -------------
# sg-admind in its test mode, watching a spool of the gate's, with stand-ins:
# apt-get records its calls; dpkg-query lists the package's registry
# defaults; runuser (the registration step: importing them as the machine
# account) records its call and registers the setup program (App Paths), as
# the real import does.
S="$T/spool"; AB="$T/abin"; CALLS="$T/admind-calls"
mkdir -p "$S/requests" "$S/replies" "$AB"; chmod 700 "$S/requests"; : > "$CALLS"
fakewin=$(wine winepath -w "$T/store-fake.exe" 2>/dev/null | tr -d '\r')
cat > "$AB/apt-get" <<EOF
#!/bin/sh
printf 'apt-get %s\n' "\$*" >> "$CALLS"
EOF
cat > "$AB/runuser" <<EOF
#!/bin/sh
printf 'runuser %s\n' "\$*" >> "$CALLS"
WINEPREFIX="$WINEPREFIX" WINEDEBUG=-all DISPLAY="$DISPLAY" "$WINE_DIR/bin/wine" reg add \
    'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\gate-setup.exe' /ve /d '$fakewin' /f >/dev/null 2>&1
EOF
cat > "$AB/dpkg-query" <<'EOF'
#!/bin/sh
printf '/usr/share/stained-glass/defaults.d/89-sg-office.reg\n'
EOF
chmod +x "$AB"/*
: > "$T/admind-run"
( while [ -e "$T/admind-run" ]; do
      SG_ADMIN_TEST=1 SG_ADMIN_SPOOL="$S" SG_ADMIN_PATH="$AB" SG_ADMIN_SYSTEM_UID="$(id -u)" \
          python3 "$HERE/admin/sg-admind" 2>>"$T/admind.log"
      sleep 0.3
  done ) & AP=$!
reg "$K\\05" /v Name /d 'Our Suite'
reg "$K\\05" /v Publisher /d 'Stained Glass OS'
reg "$K\\05" /v Tier /d ours
reg "$K\\05" /v Source /d 'ours:setup:gate-setup.exe'
reg "$K\\05" /v Package /d 'sg-office'
reg "$K\\05" /v DetectName /d 'Our Suite'
reg "$K\\06" /v Name /d 'Not Ours'
reg "$K\\06" /v Tier /d ours
reg "$K\\06" /v Source /d 'ours:setup:other-setup.exe'
reg "$K\\06" /v Package /d 'openssh-server'
# a removed package leaves its keys: App Paths naming a file that is gone
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\gate-setup.exe' /ve /d 'C:\gone\gate-setup.exe'
wineserver -w
ours() { SG_ADMIN_SPOOL="$S" SG_FAKE_KEY=OurSuite SG_FAKE_DISPLAY='Our Suite' SG_FAKE_VERSION=26.8 SG_FAKE_DIR='Our Suite' \
         wine "${SG_MUT:-$EXE}" --install "$1" >/dev/null 2>&1; }
# the mutant first (it must leave nothing behind): the package is never installed
rm -f "$G/setup.log"; : > "$CALLS"
SG_MUT="$T/mut-nopackage.exe" ours 05; ec=$?
[ "$ec" != 0 ] && ! grep -q 'install sg-office' "$CALLS" \
    && pass "MUTANT NOPACKAGE fails without the package (gate catches it)" || fail "NOPACKAGE not detected (exit $ec)"
rm -f "$G/setup.log"; : > "$CALLS"
ours 05; ec=$?
[ "$ec" = 0 ] && pass "an app whose programs are a missing system package installs (its stale App Paths entry ignored)" || fail "install 05 exit $ec: $(cat "$D.result" 2>/dev/null)"
grep -q '^apt-get .*install sg-office$' "$CALLS" && pass "the package was installed through sg-admind" || fail "apt-get calls: $(tr '\n' '|' < "$CALLS")"
grep -q '^runuser -u sgsystem -- .*sg-register /usr/share/stained-glass/defaults.d/89-sg-office.reg$' "$CALLS" \
    && pass "and registered (its registry defaults, imported as the machine account)" || fail "no registration: $(tr '\n' '|' < "$CALLS")"
grep -q '/install /quiet' "$G/setup.log" 2>/dev/null && pass "then its setup program ran (/install /quiet)" || fail "setup.log '$(cat "$G/setup.log" 2>/dev/null)'"
run --list
[ "$(kv "$(appline 05)" state)" = installed ] && pass "and it is detected as installed" || fail "after install: $(appline 05)"
: > "$CALLS"
ours 06; ec=$?
[ "$ec" != 0 ] && ! grep -q 'openssh-server' "$CALLS" \
    && pass "a package not on sg-admind's list is refused and never reaches apt" || fail "not-ours: exit $ec, calls $(tr '\n' '|' < "$CALLS")"
grep -q 'the system package failed' "$D.result" 2>/dev/null \
    && pass "and the person is told the package was not installed" || fail "result '$(cat "$D.result" 2>/dev/null)'"
rm -f "$T/admind-run"
wine reg delete "$K\\05" /f >/dev/null 2>&1; wine reg delete "$K\\06" /f >/dev/null 2>&1
wine reg delete 'HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\OurSuite' /f >/dev/null 2>&1
rm -rf "$WINEPREFIX/drive_c/Program Files/Our Suite"; wineserver -w

# --- F. the window --------------------------------------------------------------------------------
# reset the installs from D/E so every catalogue card shows Install
wine reg delete 'HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\FakeApp' /f >/dev/null 2>&1
wine reg delete 'HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\PinnedApp' /f >/dev/null 2>&1
rm -rf "$WINEPREFIX/drive_c/Program Files/Fake App" "$WINEPREFIX/drive_c/Program Files/Pinned App"
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
wineserver -w
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
rm -f "$D"
wine "$EXE" >/dev/null 2>&1 &
i=0; while ! grep -q '^window 1' "$D" 2>/dev/null && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
if grep -q '^window 1' "$D" 2>/dev/null; then
    pass "the window opens"
    sleep 0.5; import -window root "$OUT/store-open.png" 2>/dev/null
    [ "$(field show-linux)" = 0 ] && pass "the Linux tier is collapsed by default" || fail "show-linux '$(field show-linux)'"
    grep -q '^hit install 0 ' "$D" && pass "a Windows-tier app's Install button is shown" || fail "no install 0 hit"
    ! grep -q '^hit install 3 ' "$D" && pass "the Linux app is not shown until the disclosure is opened" || fail "linux install hit present while collapsed"
    set -- $(grep '^hit linuxtoggle ' "$D" | head -1)
    if [ $# -ge 5 ]; then
        xdotool mousemove "$4" "$5" click 1; sleep 0.6
        [ "$(field show-linux)" = 1 ] && pass "the Advanced disclosure opens the Linux tier" || fail "disclosure did not open (show-linux '$(field show-linux)')"
        grep -q '^hit install 3 ' "$D" && pass "the Linux app appears under the disclosure" || fail "linux app not shown after disclosure"
        import -window root "$OUT/store-linux.png" 2>/dev/null
    else fail "no Advanced disclosure hit"; fi
else fail "the window did not open: $(head -3 "$D" 2>/dev/null | tr '\n' ' ')"; fi
wine taskkill /f /im sg-store64.exe >/dev/null 2>&1

[ $RC = 0 ] && echo "store-check: all passed" || echo "store-check: FAILED"
exit $RC
