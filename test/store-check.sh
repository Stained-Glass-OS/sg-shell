#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for SG Store (sg-store64.exe): the catalogue engine, the window, and
# system packages (Linux apps, .deb files).
#
# Against a winget-pkgs-shaped source served from this machine (python3 -m
# http.server), a stand-in installer (test/sg-store-fake.c), a gate catalogue
# in HKLM\Software\Stained Glass\Store, a dpkg status file of the gate's own
# and sg-admind run unprivileged (SG_ADMIN_TEST) with a stand-in apt-get:
#
#   A. --list: the Linux apps are listed last, in their own section, and the
#      Windows build of a program comes before its Linux build
#   B. David 2026-09-30: the Windows GIMP installed made the Linux GIMP look
#      installed -- a Linux app is detected by dpkg's state only, a Windows
#      program by its own name only (as a whole word: Git is not GitHub
#      Desktop; XnView is not XnView MP)
#   C. --check-updates marks an installed app with an older version as an
#      update and names the newer version
#   D. a download whose SHA-256 is not the manifest's is refused, never run
#   E. a winget manifest install; F. an installer we can run beats a portable
#      zip of the same arch; G. a zip holding the installer (Paint.NET's) is
#      unpacked and run; H. a pinned install
#   I. a Linux app installs through the elevated copy and sg-admind's
#      apt-install, with the progress window, and is then detected
#   I2. one of ours (SG Office: ours:apt:sg-office) installs the same way, is
#      registered in the Windows side as it installs (sg-admind imports its
#      registry defaults as the machine account), is detected from dpkg, and
#      Open starts its program by its App Paths name (Run)
#   J. a .deb: the "Install a Linux package" window says what it is (name,
#      version, maker), Install stages it and sg-admind's deb-install installs
#      it; a file that is not a package is refused; File Explorer's .deb verb
#      (defaults/85-sg-store.reg) opens that window
#   E2. Open starts the installed program: its DisplayIcon program, else its
#      Start shortcut (never its uninstaller's)
#   K. the window: searching filters the whole catalogue; the Linux apps
#      category; the keyboard (Down selects, Esc back to the search box)
#
# Mutants (built here from source) -- each must turn it red:
#   SG_MUTANT_NOHASH      installs an unverified download
#   SG_MUTANT_LINUXMIXED  files Linux apps among the Windows programs
#   SG_MUTANT_LINUXBYNAME detects a Linux app by a Windows program's name (the bug)
#   SG_MUTANT_SUBSTRING   matches names anywhere ("Git" in "GitHub Desktop")
#   SG_MUTANT_NOUPDATE    never notices a newer version
#   SG_MUTANT_ANYTYPE     picks a portable zip over a runnable installer
#   SG_MUTANT_NOZIP       does not unpack a zipped installer
#   SG_MUTANT_NOSEARCH    the search box filters nothing
#   SG_MUTANT_NOCUSTOM    drops a manifest's Custom switches (Opera stopped with 103)
#   SG_MUTANT_NOLAUNCH    Open shows Apps & features instead of starting the program
#   SG_MUTANT_NOELEVATE   ignores ElevationRequirement: elevationRequired
#   SG_MUTANT_NOWOWCU     misses per-user entries under HKCU\Software\WOW6432Node
#   SG_MUTANT_NOPLUS      refuses a winget id with + (Notepad++.Notepad++)
#
# Needs wine-sg, mingw, Xvfb, xdotool, ImageMagick, python3, dpkg-deb; skips
# (77) without them. SG_STORE_EXE tests another build.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_STORE_EXE:-$HERE/build/sg-store64.exe}"
OUT="$HERE/build"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; XP=""; HP=""; AP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import python3 dpkg-deb "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-store-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    [ -n "$HP" ] && kill "$HP" 2>/dev/null
    [ -n "$AP" ] && kill "$AP" 2>/dev/null
    rm -f "$T/admind-run"; [ -n "${AP2:-}" ] && kill "$AP2" 2>/dev/null
    [ -n "${SG_KEEP:-}" ] || rm -rf "$T"
}
trap cleanup EXIT INT TERM

STORE_SRC="$HERE/src/store/main.c $HERE/src/store/catalog.c $HERE/src/store/sysinstall.c $HERE/src/browser/fetch.c $HERE/src/browser/manifest.c $HERE/src/zip/zipcore.c"
STORE_LIBS="-lwininet -lbcrypt -lshlwapi -lshell32 -lgdi32 -luser32 -ladvapi32 -lole32 -lmsimg32 -lcomdlg32"
build_mut() { # define outfile
    # shellcheck disable=SC2086
    "$MINGW" -municode -mwindows -O1 -Wno-missing-field-initializers -I"$HERE/src/browser" -I"$HERE/src/store" -I"$HERE/src/zip" \
        "-D$1" -o "$2" $STORE_SRC $STORE_LIBS 2>>"$T/cc.log"
}
for m in NOHASH LINUXMIXED LINUXBYNAME SUBSTRING NOUPDATE ANYTYPE NOZIP NOSEARCH NOCUSTOM NOLAUNCH NOELEVATE NOWOWCU NOPLUS; do
    build_mut "SG_MUTANT_$m" "$T/mut-$(echo $m | tr 'A-Z' 'a-z').exe" || fail "mutant $m does not build: $(tail -3 "$T/cc.log")"
done

# the stand-ins
"$MINGW" -municode -O1 -o "$T/store-fake.exe" "$HERE/test/sg-store-fake.c" -lole32 -luuid -lshell32 || { fail "the stand-ins do not build"; exit 1; }

# --- the source: winget-pkgs' layout on this machine ---------------------------------------------
W="$T/www"
mkdir -p "$W/api/f/Fake" "$W/api/b/Bad" "$W/raw/f/Fake/App/1.10.0" "$W/raw/b/Bad/Hash/2.0" "$W/files" \
         "$W/raw/f/Fake/Pick/2.0" "$W/raw/f/Fake/Zip/5.1" "$W/raw/f/Fake/Plus++/1.0"
cp "$T/store-fake.exe" "$W/files/store-fake.exe"
SHA=$(sha256sum "$W/files/store-fake.exe" | cut -d' ' -f1)
# a zip that holds the installer (and a readme), as Paint.NET's does
python3 - "$W/files/zipped.zip" "$T/store-fake.exe" <<'EOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    z.write(sys.argv[2], "zipped.5.1.install.x64.exe")
    z.writestr("readme.txt", "the installer is beside me\n")
EOF
ZSHA=$(sha256sum "$W/files/zipped.zip" | cut -d' ' -f1)
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
printf '[{"name": "1.9.0", "type": "dir"}, {"name": "1.10.0", "type": "dir"}]' > "$W/api/f/Fake/App"
printf '[{"name": "2.0", "type": "dir"}]' > "$W/api/b/Bad/Hash"
printf '[{"name": "2.0", "type": "dir"}]' > "$W/api/f/Fake/Pick"
printf '[{"name": "5.1", "type": "dir"}]' > "$W/api/f/Fake/Zip"
printf '[{"name": "1.0", "type": "dir"}]' > "$W/api/f/Fake/Plus++"
# an id with + in it, as Notepad++.Notepad++
cat > "$W/raw/f/Fake/Plus++/1.0/Fake.Plus++.installer.yaml" <<EOF
PackageIdentifier: Fake.Plus++
PackageVersion: 1.0
InstallerType: nullsoft
Installers:
- Architecture: x64
  InstallerSwitches:
    Silent: /S /plus-id
  InstallerUrl: http://127.0.0.1:$PORT/files/store-fake.exe
  InstallerSha256: $SHA
EOF
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
# the portable zip first, as winget-pkgs lists Notepad++'s: same arch, same scope
cat > "$W/raw/f/Fake/Pick/2.0/Fake.Pick.installer.yaml" <<EOF
PackageIdentifier: Fake.Pick
PackageVersion: 2.0
Installers:
- Architecture: x64
  InstallerType: zip
  NestedInstallerType: portable
  Scope: machine
  InstallerUrl: http://127.0.0.1:$PORT/files/portable.zip
  InstallerSha256: 2222222222222222222222222222222222222222222222222222222222222222
- Architecture: x64
  InstallerType: nullsoft
  Scope: machine
  InstallerSwitches:
    Silent: /S /picked-nullsoft
    Custom: /gate-custom=1
  ElevationRequirement: elevationRequired
  InstallerUrl: http://127.0.0.1:$PORT/files/store-fake.exe
  InstallerSha256: $SHA
ManifestType: installer
EOF
cat > "$W/raw/f/Fake/Zip/5.1/Fake.Zip.installer.yaml" <<EOF
PackageIdentifier: Fake.Zip
PackageVersion: 5.1
InstallerType: zip
NestedInstallerType: exe
NestedInstallerFiles:
- RelativeFilePath: zipped.5.1.install.x64.exe
InstallerSwitches:
  Silent: /auto /from-zip
Installers:
- Architecture: x64
  InstallerUrl: http://127.0.0.1:$PORT/files/zipped.zip
  InstallerSha256: $ZSHA
ManifestType: installer
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

# --- sg-admind, unprivileged, with a stand-in apt-get --------------------------------------------
S="$T/spool"; B="$T/bin"; DEBS="$T/debs"
mkdir -p "$S/requests" "$S/replies" "$B" "$DEBS" "$T/work"
chmod 700 "$S/requests" "$DEBS"
: > "$T/dpkg-status"; : > "$T/installed"; : > "$T/apt.log"
# apt-get: records how it was called, reports progress on APT::Status-Fd, and
# "installs": the package goes into dpkg's status (the store's view) and the
# installed list (the stand-in dpkg-query's)
cat > "$B/apt-get" <<EOF
#!/bin/sh
fd=; last=
for a in "\$@"; do case \$a in APT::Status-Fd=*) fd=\${a#APT::Status-Fd=} ;; esac; last=\$a; done
printf '%s\n' "\$*" >> "$T/apt.log"
case " \$* " in *" update "*) exit 0 ;; esac
[ -n "\$fd" ] && eval "printf 'dlstatus:x:40:Downloading the gate package\npmstatus:x:30:Unpacking the gate package\n' >&\$fd"
sleep 1
[ -n "\$fd" ] && eval "printf 'pmstatus:x:90:Setting up the gate package\n' >&\$fd"
if [ -f "\$last" ]; then name=\$(dpkg-deb -f "\$last" Package); ver=\$(dpkg-deb -f "\$last" Version)
else name=\$last; ver=9.9-gate; fi
printf 'Package: %s\nStatus: install ok installed\nVersion: %s\n\n' "\$name" "\$ver" >> "$T/dpkg-status"
printf '%s %s\n' "\$name" "\$ver" >> "$T/installed"
EOF
cat > "$B/dpkg-query" <<EOF
#!/bin/sh
[ "\$1" = -L ] && { printf '/usr/share/doc/%s\n/usr/share/stained-glass/defaults.d/89-sg-office.reg\n' "\$2"; exit 0; }
eval "pkg=\\\${\$#}"
v=\$(sed -n "s/^\$pkg //p" "$T/installed" | tail -1)
[ -n "\$v" ] || exit 1
printf 'installed %s' "\$v"
EOF
# runuser: the registration step (importing a package's registry defaults
# as the machine account) -- recorded
cat > "$B/runuser" <<EOF
#!/bin/sh
printf 'runuser %s\n' "\$*" >> "$T/apt.log"
EOF
chmod +x "$B"/*
SG_ADMIN_SYSTEM_UID=$(id -u)
export SG_ADMIN_TEST=1 SG_ADMIN_SPOOL="$S" SG_ADMIN_PATH="$B" SG_ADMIN_SYSTEM_UID SG_ADMIN_DEBS="$DEBS" SG_ADMIN_WORK="$T/work"
( while :; do python3 "$HERE/admin/sg-admind" 2>>"$T/admind.log"; sleep 0.3; done ) & AP=$!
export SG_STORE_DIRECT=1 SG_DEBINFO="$HERE/src/store/sg-debinfo"
SG_DPKG_STATUS=$(wine winepath -w "$T/dpkg-status" 2>/dev/null | tr -d '\r'); export SG_DPKG_STATUS

# --- the gate's catalogue ------------------------------------------------------------------------
K='HKLM\Software\Stained Glass\Store\Apps'
wine reg delete "$K" /f >/dev/null 2>&1
app() { # ord name category tier source detect
    reg "$K\\$1" /v Name /d "$2"; reg "$K\\$1" /v Publisher /d 'The gate'
    reg "$K\\$1" /v Description /d "$2, a gate app."; reg "$K\\$1" /v Category /d "$3"
    reg "$K\\$1" /v Tier /d "$4"; reg "$K\\$1" /v Source /d "$5"
    [ -n "${6:-}" ] && reg "$K\\$1" /v DetectName /d "$6"
}
app 01 'Fake App' Browsers windows 'winget:Fake.App' 'Fake App'
app 02 'Pinned App' Utilities windows "pin:http://127.0.0.1:$PORT/files/store-fake.exe|$SHA|nullsoft|/S" 'Pinned App'
reg "$K\\02" /v PinVersion /d '3.0'
app 04 'Tampered App' Utilities windows 'winget:Bad.Hash' 'Bad'
app 05 'GIMP' Graphics windows 'winget:Fake.Gimp' 'GIMP'
app 06 'Git' Development windows 'winget:Fake.Git' 'Git'
app 07 'XnView' Graphics windows 'winget:Fake.XnView' 'XnView|!XnView MP'
app 08 'Pick App' Utilities windows 'winget:Fake.Pick' 'Pick App'
app 09 'Zip App' Graphics windows 'winget:Fake.Zip' 'Zip App'
app L1 'GIMP' Graphics linux 'linux:apt:gimp' 'GIMP'
app L2 'Gate Tool' Utilities linux 'linux:apt:sg-gate-tool'
wineserver -w

G="$WINEPREFIX/drive_c/gate"; mkdir -p "$G"
export SG_STORE_DUMP='C:\gate\dump'
D="$G/dump"
field() { sed -n "s/^$1 //p" "${2:-$D}" 2>/dev/null | head -1; }
appline() { grep "^app $1 " "$D" 2>/dev/null | head -1; }
kv() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }
run() { rm -f "$D" "$D.result"; wine "${SG_MUT:-$EXE}" "$@" >/dev/null 2>&1; }
# the list's order, as ordinals
view() { field view | cut -d' ' -f2-; }
pos() { echo " $(view) " | tr ' ' '\n' | grep -n "^$1\$" | cut -d: -f1; }
uninstall() { reg "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\$1" /v DisplayName /d "$2"
              reg "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\$1" /v DisplayVersion /d "$3"; }
ununinstall() { wine reg delete "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\$1" /f >/dev/null 2>&1; }

# --- A. the Linux apps: listed last, in their own section ---------------------------------------
run --list
[ "$(field catalog)" = 10 ] && pass "the catalogue loads (10 apps)" || fail "catalog '$(field catalog)'"
[ "$(kv "$(appline L1)" section)" = Linux ] && pass "a Linux app is in the Linux apps section" || fail "L1: $(appline L1)"
[ "$(kv "$(appline 05)" section)" = Graphics ] && pass "a Windows program is in its category" || fail "05: $(appline 05)"
[ -n "$(pos L1)" ] && [ "$(pos L1)" -gt "$(pos 09)" ] && [ "$(pos L1)" -gt "$(pos 02)" ] && [ "$(pos L2)" -gt "$(pos 02)" ] \
    && pass "the Linux apps come after every other app" || fail "view: $(view)"
[ "$(pos 05)" -lt "$(pos L1)" ] && pass "the Windows GIMP is listed (suggested) before the Linux GIMP" || fail "view: $(view)"
SG_MUT="$T/mut-linuxmixed.exe" run --list
[ "$(pos L1)" -lt "$(pos 09)" ] 2>/dev/null && pass "MUTANT LINUXMIXED files the Linux GIMP among the graphics apps (gate catches it)" \
    || fail "LINUXMIXED not detected: $(view)"

# --- B. detection: each kind by its own evidence -------------------------------------------------
uninstall GIMP_is1 'GIMP 3.2.6' 3.2.6
uninstall GitHubDesktop 'GitHub Desktop' 3.4.0
uninstall XnViewMP 'XnView MP 1.12.1' 1.12.1
wineserver -w
run --list
[ "$(kv "$(appline 05)" state)" = installed ] && pass "the Windows GIMP is detected by its Uninstall entry" || fail "05: $(appline 05)"
[ "$(kv "$(appline L1)" state)" = not-installed ] && pass "the Windows GIMP does not make the Linux GIMP look installed (David's report)" \
    || fail "L1: $(appline L1)"
[ "$(kv "$(appline 06)" state)" = not-installed ] && pass "\"GitHub Desktop\" is not Git" || fail "06: $(appline 06)"
[ "$(kv "$(appline 07)" state)" = not-installed ] && pass "\"XnView MP\" is not XnView" || fail "07: $(appline 07)"
SG_MUT="$T/mut-linuxbyname.exe" run --list
[ "$(kv "$(appline L1)" state)" = installed ] && pass "MUTANT LINUXBYNAME shows the Linux GIMP installed (gate catches it)" || fail "LINUXBYNAME not detected"
SG_MUT="$T/mut-substring.exe" run --list
[ "$(kv "$(appline 06)" state)" = installed ] && pass "MUTANT SUBSTRING takes GitHub Desktop for Git (gate catches it)" || fail "SUBSTRING not detected"
printf 'Package: gimp\nStatus: install ok installed\nVersion: 3.0.4-3\n\nPackage: other\nStatus: deinstall ok config-files\nVersion: 1\n\n' > "$T/dpkg-status"
uninstall Git_is1 'Git' 2.55.0
uninstall XnView_is1 'XnView 2.52.7' 2.52.7
wineserver -w
run --list
[ "$(kv "$(appline L1)" state)" = installed ] && [ "$(kv "$(appline L1)" installed)" = 3.0.4-3 ] \
    && pass "the Linux GIMP is detected from dpkg's status (3.0.4-3)" || fail "L1: $(appline L1)"
[ "$(kv "$(appline 06)" state)" = installed ] && [ "$(kv "$(appline 07)" state)" = installed ] \
    && pass "Git and XnView are detected by their own names" || fail "06/07: $(appline 06) / $(appline 07)"
: > "$T/dpkg-status"
for k in GIMP_is1 GitHubDesktop XnViewMP Git_is1 XnView_is1; do ununinstall $k; done
wineserver -w

# a 32-bit installer's per-user entry: Wine files it under HKCU\Software\WOW6432Node
# (Kdenlive, BleachBit said "Installed." and stayed "Install")
reg 'HKCU\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Git' /v DisplayName /d 'Git'
reg 'HKCU\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Git' /v DisplayVersion /d '2.55.0'
wineserver -w
run --list
[ "$(kv "$(appline 06)" state)" = installed ] && pass "a per-user entry under HKCU\\Software\\WOW6432Node is seen (Kdenlive, BleachBit)" || fail "06 (HKCU WOW6432Node): $(appline 06)"
SG_MUT="$T/mut-nowowcu.exe" run --list
[ "$(kv "$(appline 06)" state)" = not-installed ] && pass "MUTANT NOWOWCU misses it (gate catches it)" || fail "NOWOWCU not detected: $(appline 06)"
wine reg delete 'HKCU\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Git' /f >/dev/null 2>&1
wineserver -w

# --- C. update detection --------------------------------------------------------------------------
uninstall FakeApp 'Fake App' 1.0.0
wineserver -w
run --check-updates
a01=$(appline 01)
[ "$(kv "$a01" installed)" = 1.0.0 ] && pass "an installed app's version is detected (1.0.0)" || fail "01 installed '$(kv "$a01" installed)'"
[ "$(kv "$a01" available)" = 1.10.0 ] && pass "the newer available version is resolved (1.10.0)" || fail "01 available '$(kv "$a01" available)'"
[ "$(kv "$a01" state)" = update ] && pass "the app is marked as an update" || fail "01 state '$(kv "$a01" state)'"
SG_MUT="$T/mut-noupdate.exe" run --check-updates
[ "$(kv "$(appline 01)" state)" != update ] && pass "MUTANT NOUPDATE misses the update (gate catches it)" || fail "NOUPDATE not detected by the gate"
ununinstall FakeApp; wineserver -w

# --- D. a tampered download is refused ------------------------------------------------------------
rm -f "$G/setup.log"
run --install 04
[ ! -e "$G/setup.log" ] && pass "a download whose SHA-256 is not the manifest's is refused, the installer never runs" \
    || fail "the tampered installer ran: $(cat "$G/setup.log" 2>/dev/null)"
grep -qi 'SHA-256' "$D.result" 2>/dev/null && pass "and the reason names the SHA-256 mismatch" || fail "result '$(cat "$D.result" 2>/dev/null)'"
rm -f "$G/setup.log"
SG_MUT="$T/mut-nohash.exe" run --install 04
[ -e "$G/setup.log" ] && pass "MUTANT NOHASH runs the unverified installer (gate catches it)" || fail "NOHASH not detected by the gate"
ununinstall FakeApp; rm -f "$G/setup.log"; wineserver -w

# --- E. a winget manifest install -----------------------------------------------------------------
SG_FAKE_KEY=FakeApp SG_FAKE_DISPLAY='Fake App' SG_FAKE_VERSION=1.10.0 SG_FAKE_DIR='Fake App' \
    wine "$EXE" --install 01 >/dev/null 2>&1; ec=$?
[ "$ec" = 0 ] && pass "--install through the manifest succeeds" || fail "install 01 exit $ec: $(cat "$D.result" 2>/dev/null)"
grep -q '/S /gate-silent' "$G/setup.log" 2>/dev/null && pass "run with the manifest's silent switches" || fail "setup.log '$(cat "$G/setup.log" 2>/dev/null)'"
[ -f "$WINEPREFIX/drive_c/Program Files/Fake App/app.exe" ] && pass "the app is installed" || fail "app.exe not installed"
grep -q '^elevated 01 0' "$D.result" 2>/dev/null && pass "as the user: its manifest asks for no administrator" || fail "01 elevated: $(cat "$D.result" 2>/dev/null)"
run --list
[ "$(kv "$(appline 01)" state)" = installed ] && [ "$(kv "$(appline 01)" installed)" = 1.10.0 ] \
    && pass "and is then detected as installed (1.10.0)" || fail "after install: $(appline 01)"

# --- F. an installer we can run beats a portable zip ---------------------------------------------
rm -f "$G/setup.log"
SG_FAKE_KEY=PickApp SG_FAKE_DISPLAY='Pick App' SG_FAKE_VERSION=2.0 SG_FAKE_DIR='Pick App' \
    wine "$EXE" --install 08 >/dev/null 2>&1; ec=$?
[ "$ec" = 0 ] && grep -q '/picked-nullsoft' "$G/setup.log" 2>/dev/null \
    && pass "the runnable installer is picked over a portable zip of the same arch" || fail "install 08 exit $ec: $(cat "$D.result" "$G/setup.log" 2>/dev/null)"
grep -q '/S /picked-nullsoft /gate-custom=1' "$G/setup.log" 2>/dev/null \
    && pass "and run with its Custom switches after the silent ones (Opera's /allusers)" || fail "no Custom switches: $(cat "$G/setup.log" 2>/dev/null)"
grep -q '^elevated 08 1' "$D.result" 2>/dev/null \
    && pass "an installer whose manifest says elevationRequired is started as an administrator (foobar2000, Mp3tag)" \
    || fail "elevationRequired not elevated: $(cat "$D.result" 2>/dev/null)"
rm -f "$G/setup.log"; ununinstall PickApp
SG_FAKE_KEY=PickApp SG_FAKE_DISPLAY='Pick App' SG_FAKE_VERSION=2.0 SG_FAKE_DIR='Pick App' \
    wine "$T/mut-noelevate.exe" --install 08 >/dev/null 2>&1
grep -q '^elevated 08 0' "$D.result" 2>/dev/null \
    && pass "MUTANT NOELEVATE runs it as the user (gate catches it)" || fail "NOELEVATE not detected: $(cat "$D.result" 2>/dev/null)"
rm -f "$G/setup.log"; ununinstall PickApp
SG_FAKE_KEY=PickApp SG_FAKE_DISPLAY='Pick App' SG_FAKE_VERSION=2.0 SG_FAKE_DIR='Pick App' \
    wine "$T/mut-nocustom.exe" --install 08 >/dev/null 2>&1
grep -q '/picked-nullsoft' "$G/setup.log" 2>/dev/null && ! grep -q 'gate-custom' "$G/setup.log" \
    && pass "MUTANT NOCUSTOM drops the Custom switches (gate catches it)" || fail "NOCUSTOM not detected: $(cat "$G/setup.log" 2>/dev/null)"
rm -f "$G/setup.log"; ununinstall PickApp
SG_MUT="$T/mut-anytype.exe" run --install 08
[ ! -e "$G/setup.log" ] && pass "MUTANT ANYTYPE picks the portable zip and installs nothing (gate catches it)" || fail "ANYTYPE not detected"

# --- E2. Open starts the installed program (it opened Apps & features) ---------------------------
# Fake App (01, still installed): its Uninstall entry's DisplayIcon is its program.
# Pick App (08): the DisplayIcon is its uninstaller; its Start shortcut is the way.
launched() { i=0; while ! grep -q "$1" "$G/launch.log" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.25; i=$((i + 1)); done
             grep -q "$1" "$G/launch.log" 2>/dev/null; }
rm -f "$G/launch.log"
run --open 01
launched 'app.exe' && pass "Open starts the program its Uninstall entry names (DisplayIcon)" \
    || fail "Open 01 started nothing: $(cat "$G/launch.log" 2>/dev/null)"
rm -f "$G/launch.log"
SG_FAKE_LINK=1 SG_FAKE_KEY=PickApp SG_FAKE_DISPLAY='Pick App' SG_FAKE_VERSION=2.0 SG_FAKE_DIR='Pick App' \
    wine "$EXE" --install 08 >/dev/null 2>&1
run --open 08
launched 'from-shortcut' && ! grep -q 'uninstall' "$G/launch.log" \
    && pass "no program in DisplayIcon (an uninstaller): its Start shortcut, not the Uninstall one" \
    || fail "Open 08: $(cat "$G/launch.log" 2>/dev/null)"
rm -f "$G/launch.log"
wine "$T/mut-nolaunch.exe" --open 01 >/dev/null 2>&1; sleep 3
[ ! -e "$G/launch.log" ] && pass "MUTANT NOLAUNCH opens Apps & features, never the program (gate catches it)" \
    || fail "NOLAUNCH not detected: $(cat "$G/launch.log" 2>/dev/null)"
wine taskkill /f /im sg-settings64.exe >/dev/null 2>&1
rm -f "$G/setup.log"; ununinstall PickApp
rm -f "$WINEPREFIX/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs/"*Pick\ App*.lnk

# --- G. a zip holding the installer ---------------------------------------------------------------
rm -f "$G/setup.log"
SG_FAKE_KEY=ZipApp SG_FAKE_DISPLAY='Zip App' SG_FAKE_VERSION=5.1 SG_FAKE_DIR='Zip App' \
    wine "$EXE" --install 09 >/dev/null 2>&1; ec=$?
[ "$ec" = 0 ] && grep -q '/auto /from-zip' "$G/setup.log" 2>/dev/null \
    && pass "a zipped installer (checked by the zip's SHA-256) is unpacked and run" || fail "install 09 exit $ec: $(cat "$D.result" "$G/setup.log" 2>/dev/null)"
[ -f "$WINEPREFIX/drive_c/Program Files/Zip App/app.exe" ] && pass "the zipped app is installed" || fail "zip app not installed"
rm -f "$G/setup.log"; ununinstall ZipApp
SG_MUT="$T/mut-nozip.exe" run --install 09
[ ! -e "$G/setup.log" ] && pass "MUTANT NOZIP installs nothing (gate catches it)" || fail "NOZIP not detected"

# --- H. a pinned install --------------------------------------------------------------------------
rm -f "$G/setup.log"
SG_FAKE_KEY=PinnedApp SG_FAKE_DISPLAY='Pinned App' SG_FAKE_VERSION=3.0 SG_FAKE_DIR='Pinned App' \
    wine "$EXE" --install 02 >/dev/null 2>&1; ec=$?
[ "$ec" = 0 ] && pass "a pinned entry installs (SHA-256 checked from the catalogue)" || fail "install 02 exit $ec: $(cat "$D.result" 2>/dev/null)"
[ -f "$WINEPREFIX/drive_c/Program Files/Pinned App/app.exe" ] && pass "the pinned app is installed" || fail "pinned app.exe not installed"
# a winget id with + in it (Notepad++.Notepad++ was "not a package name")
reg "$K\\04" /v Source /d 'winget:Fake.Plus++'
rm -f "$G/setup.log"
SG_FAKE_KEY=PlusApp SG_FAKE_DISPLAY='Bad' SG_FAKE_VERSION=1.0 SG_FAKE_DIR='Plus App' wine "$EXE" --install 04 >/dev/null 2>&1; ec=$?
[ "$ec" = 0 ] && grep -q '/S /plus-id' "$G/setup.log" 2>/dev/null && pass "a winget id with + in it installs (Notepad++)" \
    || fail "install of Fake.Plus++ exit $ec: $(cat "$D.result" "$G/setup.log" 2>/dev/null)"
rm -f "$G/setup.log"
SG_FAKE_KEY=PlusApp SG_FAKE_DISPLAY='Bad' SG_FAKE_VERSION=1.0 SG_FAKE_DIR='Plus App' wine "$T/mut-noplus.exe" --install 04 >/dev/null 2>&1
grep -q 'not a package name' "$D.result" 2>/dev/null && [ ! -e "$G/setup.log" ] && pass "MUTANT NOPLUS refuses the id (gate catches it)" \
    || fail "NOPLUS not detected: $(cat "$D.result" 2>/dev/null)"
reg "$K\\04" /v Source /d 'winget:Bad.Hash'; ununinstall PlusApp
ununinstall FakeApp; ununinstall PinnedApp
rm -rf "$WINEPREFIX/drive_c/Program Files/Plus App" "$WINEPREFIX/drive_c/Program Files/Fake App" "$WINEPREFIX/drive_c/Program Files/Pinned App" "$WINEPREFIX/drive_c/Program Files/Pick App" "$WINEPREFIX/drive_c/Program Files/Zip App"
wineserver -w

# the desktop the windows below live on
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
wineserver -w
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

# click the (screen) centre the dump FILE names for "hit WHAT"
click() { xy=$(grep "^hit $2 " "$1" 2>/dev/null | tail -1 | awk '{ print $(NF-1), $NF }'); [ -n "$xy" ] || return 1
          xdotool mousemove "${xy% *}" "${xy#* }" click 1; }
waitfor() { i=0; while ! grep -q "$2" "$1" 2>/dev/null && [ $i -lt "${3:-80}" ]; do sleep 0.25; i=$((i + 1)); done; grep -q "$2" "$1" 2>/dev/null; }

# --- I. a Linux app, through the consent (direct here) and sg-admind ------------------------------
rm -f "$D" "$D.result" "$D.sys"
wine "$EXE" --install L2 >/dev/null 2>&1 & IP=$!
if waitfor "$D.sys" '^result ' 120; then
    grep -q '^result ok' "$D.sys" && pass "the elevated copy's request is answered: installed" || fail "result: $(grep '^result' "$D.sys")"
    grep -q '^progress [0-9]* .*gate package' "$D.sys" && pass "apt's progress reaches the progress window" || fail "no progress: $(cat "$D.sys")"
    import -window root "$OUT/store-apt-progress.png" 2>/dev/null
    grep -q 'install sg-gate-tool' "$T/apt.log" && pass "sg-admind ran apt-get install sg-gate-tool" || fail "apt.log: $(cat "$T/apt.log")"
    sleep 0.5; click "$D.sys" close
else fail "the elevated install did not finish: $(cat "$D.sys" 2>/dev/null) / $(tail -3 "$T/admind.log")"; fi
i=0; while kill -0 $IP 2>/dev/null && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
kill $IP 2>/dev/null; wait $IP 2>/dev/null; ec=$?
[ "$ec" = 0 ] && grep -q '^result L2 ok' "$D.result" 2>/dev/null && pass "the store's --install of the Linux app succeeds" || fail "install L2 exit $ec: $(cat "$D.result" 2>/dev/null)"
run --list
[ "$(kv "$(appline L2)" state)" = installed ] && [ "$(kv "$(appline L2)" installed)" = 9.9-gate ] \
    && pass "and it is then detected from dpkg's status" || fail "L2 after: $(appline L2)"

# --- I2. one of ours: SG Office's way -------------------------------------------------------------
reg "$K\\O5" /v Name /d 'Our Suite'
reg "$K\\O5" /v Publisher /d 'Stained Glass OS'
reg "$K\\O5" /v Tier /d ours
reg "$K\\O5" /v Source /d 'ours:apt:sg-office'
reg "$K\\O5" /v DetectName /d 'Our Suite'
reg "$K\\O5" /v Run /d 'gate-suite.exe'
cp "$T/store-fake.exe" "$G/app.exe"
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\gate-suite.exe' /ve /d 'C:\gate\app.exe'
sleep 0.5
run --list
[ "$(kv "$(appline O5)" tier)" = ours ] && [ "$(kv "$(appline O5)" state)" = not-installed ] \
    && pass "our own suite (ours:apt) is listed, not installed" || fail "O5 before: $(appline O5)"
rm -f "$D" "$D.result" "$D.sys"; : > "$T/apt.log"
wine "$EXE" --install O5 >/dev/null 2>&1 & IP=$!
if waitfor "$D.sys" '^result ' 120; then
    grep -q '^result ok' "$D.sys" && pass "it installs through the elevated copy and sg-admind's apt-install" || fail "O5 result: $(grep '^result' "$D.sys")"
    sleep 0.5; click "$D.sys" close
else fail "the install of our suite did not finish: $(cat "$D.sys" 2>/dev/null) / $(tail -3 "$T/admind.log")"; fi
i=0; while kill -0 $IP 2>/dev/null && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
kill $IP 2>/dev/null; wait $IP 2>/dev/null
grep -q ' install sg-office$' "$T/apt.log" && pass "apt-get install sg-office (it brings the editors)" || fail "apt.log: $(cat "$T/apt.log")"
grep -q '^runuser -u sgsystem -- .*sg-register /usr/share/stained-glass/defaults.d/89-sg-office.reg$' "$T/apt.log" \
    && pass "registered as it installs (its registry defaults, imported as the machine account): no restart" \
    || fail "not registered: $(cat "$T/apt.log")"
run --list
[ "$(kv "$(appline O5)" state)" = installed ] && pass "and it is detected from dpkg's status" || fail "O5 after: $(appline O5)"
rm -f "$G/launch.log"
run --open O5
launched 'app.exe' && pass "Open starts our suite's program by its App Paths name (Run)" \
    || fail "Open O5 started nothing: $(cat "$G/launch.log" 2>/dev/null)"
rm -f "$G/launch.log"
wine "$T/mut-nolaunch.exe" --open O5 >/dev/null 2>&1; sleep 3
[ ! -e "$G/launch.log" ] && pass "MUTANT NOLAUNCH does not start it (gate catches it)" || fail "NOLAUNCH not detected for O5"
wine taskkill /f /im sg-settings64.exe >/dev/null 2>&1
wine reg delete "$K\\O5" /f >/dev/null 2>&1

# --- J. a .deb file -------------------------------------------------------------------------------
P="$T/pkg"; mkdir -p "$P/DEBIAN" "$P/usr/share/doc/sg-gate-hello"
cat > "$P/DEBIAN/control" <<'EOF'
Package: sg-gate-hello
Version: 1.2-3
Architecture: all
Maintainer: Gate Maker <gate@example.org>
Installed-Size: 12
Homepage: https://example.org/hello
Description: Hello from the SG Store gate
 A package the gate builds.
EOF
echo hello > "$P/usr/share/doc/sg-gate-hello/README"
dpkg-deb --root-owner-group --build "$P" "$G/hello.deb" >/dev/null 2>&1 || fail "the gate's .deb does not build"
printf 'not a package\n' > "$G/notadeb.deb"
rm -f "$D" "$D.sys"
wine "$EXE" --deb 'C:\gate\hello.deb' >/dev/null 2>&1 & DP=$!
if waitfor "$D" '^hit install ' 80; then
    [ "$(field valid)" = 1 ] && [ "$(field package)" = sg-gate-hello ] && [ "$(field version)" = 1.2-3 ] \
        && pass "the .deb window names the package and version (sg-gate-hello 1.2-3)" || fail "deb dump: $(cat "$D")"
    [ "$(field maker)" = 'Gate Maker <gate@example.org>' ] && pass "and who made it" || fail "maker '$(field maker)'"
    import -window root "$OUT/store-deb.png" 2>/dev/null
    : > "$T/apt.log"
    click "$D" install
    if waitfor "$D.sys" '^result ' 120; then
        grep -q '^result ok' "$D.sys" && pass "Install: the elevated copy stages it and sg-admind installs it" || fail "deb result: $(grep '^result' "$D.sys") / $(tail -3 "$T/admind.log")"
        grep -q 'install .*/package.deb' "$T/apt.log" && pass "with apt-get install of sg-admind's own copy (dependencies come too)" || fail "apt.log: $(cat "$T/apt.log")"
        [ -z "$(ls -A "$DEBS")" ] && pass "and the staged copy is gone" || fail "left in the staging folder: $(ls "$DEBS")"
        sleep 0.5; click "$D.sys" close
        waitfor "$D" '^result 0 ' 40 && pass "the .deb window says it is installed: $(field result | cut -d' ' -f2-)" || fail "deb window result: $(cat "$D")"
        import -window root "$OUT/store-deb-done.png" 2>/dev/null
    else fail "the .deb install did not finish: $(cat "$D.sys" 2>/dev/null) / $(tail -3 "$T/admind.log")"; fi
    click "$D" close
else fail "the .deb window did not open: $(cat "$D" 2>/dev/null)"; fi
sleep 0.5; kill $DP 2>/dev/null; wait $DP 2>/dev/null
# not a package: refused, no Install
rm -f "$D"
wine "$EXE" --deb 'C:\gate\notadeb.deb' >/dev/null 2>&1 & DP=$!
if waitfor "$D" '^hit close ' 60; then
    [ "$(field valid)" = 0 ] && ! grep -q '^hit install ' "$D" && pass "a file that is not a package is refused (no Install)" || fail "notadeb: $(cat "$D")"
    grep -q '^error .*not a Linux software package' "$D" && pass "and says why" || fail "error '$(field error)'"
    click "$D" close
else fail "the refusal window did not open: $(cat "$D" 2>/dev/null)"; fi
sleep 0.5; kill $DP 2>/dev/null; wait $DP 2>/dev/null
# File Explorer's verb: HKCR\.deb from defaults/85-sg-store.reg, pointed at this build
wine regedit /S "$(wine winepath -w "$HERE/defaults/85-sg-store.reg" 2>/dev/null | tr -d '\r')" >/dev/null 2>&1
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
cmd=$(wine reg query 'HKLM\Software\Classes\SG.LinuxPackage\shell\open\command' /ve 2>/dev/null | tr -d '\r' | sed -n 's/.*REG_SZ *//p')
case "$cmd" in *sg-store64.exe*--deb*'%1'*) pass "HKCR\\.deb installs with SG Store (--deb \"%1\")" ;; *) fail ".deb verb: '$cmd'" ;; esac
reg 'HKLM\Software\Classes\SG.LinuxPackage\shell\open\command' /ve /d "\"$winexe\" --deb \"%1\""
sleep 0.5
rm -f "$D"
wine start /unix "$G/hello.deb" >/dev/null 2>&1
if waitfor "$D" '^deb-window 1' 80; then pass "double-clicking a .deb (ShellExecute) opens the Install a Linux package window"
    click "$D" cancel
else fail "ShellExecute of a .deb did not open the window: $(cat "$D" 2>/dev/null)"; fi
sleep 0.5; wine taskkill /f /im sg-store64.exe >/dev/null 2>&1
wine reg delete "$K" /f >/dev/null 2>&1      # the real catalogue came with the .reg: back to the gate's
sleep 0.5

# --- K. the window: search, the Linux apps category, the keyboard ---------------------------------
app 01 'Fake App' Browsers windows 'winget:Fake.App' 'Fake App'
app 05 'GIMP' Graphics windows 'winget:Fake.Gimp' 'GIMP'
app 06 'Git' Development windows 'winget:Fake.Git' 'Git'
app 07 'XnView' Graphics windows 'winget:Fake.XnView' 'XnView|!XnView MP'
app L1 'GIMP' Graphics linux 'linux:apt:gimp' 'GIMP'
app L2 'Gate Tool' Utilities linux 'linux:apt:sg-gate-tool'
reg "$K\\06" /v Description /d 'Distributed version control for source code.'
: > "$T/dpkg-status"
sleep 0.5
window_checks() { # exe label -- returns the view after typing "gimp"
    rm -f "$D"
    wine "$1" >/dev/null 2>&1 &
    waitfor "$D" '^window 1' 60 || { echo "NOWINDOW"; return; }
    sleep 0.8
    click "$D" "search -"; sleep 0.4
    xdotool type --delay 60 gimp; sleep 1
    field view
}
v=$(window_checks "$EXE")
if [ "$v" != NOWINDOW ]; then
    pass "the window opens"
    import -window root "$OUT/store-search.png" 2>/dev/null
    [ "$(field search)" = gimp ] && pass "typing goes to the search box" || fail "search '$(field search)'"
    [ "$v" = "2 05 L1" ] && pass "searching \"gimp\" lists both GIMPs, the Windows one first ($v)" || fail "view after 'gimp': '$v'"
    xdotool key Escape; sleep 0.3; xdotool type --delay 60 'source code'; sleep 1
    [ "$(field view)" = "1 06" ] && pass "search looks in descriptions too (\"source code\" -> Git)" || fail "view '$(field view)'"
    xdotool key Escape; sleep 0.6
    [ "$(field view | cut -d' ' -f1)" = 6 ] && pass "Esc clears the search" || fail "after Esc: '$(field view)' search '$(field search)'"
    click "$D" "category Linux" && sleep 0.8
    [ "$(field category)" = 'Linux apps' ] && [ "$(field view)" = "2 L2 L1" ] && pass "the Linux apps category lists only the Linux apps" \
        || fail "category '$(field category)' view '$(field view)'"
    import -window root "$OUT/store-linux.png" 2>/dev/null
    click "$D" "category All" && sleep 0.6
    xdotool key Tab; sleep 0.4; xdotool key Down; sleep 0.6
    first=$(field view | cut -d' ' -f2); sel=$(field selected)
    [ "$sel" != - ] && [ -n "$sel" ] && pass "Down in the list selects an app ($sel)" || fail "selected '$sel' (first $first)"
    xdotool key Escape; sleep 0.5
    [ "$(field focus)" = search ] && pass "Esc in the list goes back to the search box" || fail "focus '$(field focus)'"
    # full width: the cards take two columns, not one narrow one in the middle
    xdotool key super+Up; sleep 1.5
    xdotool key Down; sleep 0.6
    [ "$(field columns)" = 2 ] && pass "a wide window shows the cards in two columns" || fail "columns '$(field columns)' at full width"
    import -window root "$OUT/store-wide.png" 2>/dev/null
else fail "the window did not open"; fi
wine taskkill /f /im sg-store64.exe >/dev/null 2>&1; sleep 0.5
v=$(window_checks "$T/mut-nosearch.exe")
[ "$v" != NOWINDOW ] && [ "$v" != "2 05 L1" ] && pass "MUTANT NOSEARCH lists everything after \"gimp\" (gate catches it)" || fail "NOSEARCH not detected ('$v')"
wine taskkill /f /im mut-nosearch.exe >/dev/null 2>&1

[ $RC = 0 ] && echo "store-check: all passed" || echo "store-check: FAILED"
exit $RC
