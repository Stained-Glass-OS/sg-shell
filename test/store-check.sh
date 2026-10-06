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
#      unpacked and run; H. a pinned install; H2. an IExpress package of MSIs;
#      H3. a runtime DLL out of a self-extracting package's cabinet (the
#      Visual Basic 6 runtime): installed, registered, detected, its page
#      (--page) with its licence and no Open, Uninstall
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
#   L. David 2026-10-01: an app with a Windows and a Linux build is one card
#      (no duplicates), suggesting the Linux build unless Prefer=windows, the
#      installed build first; its picture (Icon) is fetched, cached and drawn;
#      a second Install waits its turn (the queue); Uninstall runs a Windows
#      program's QuietUninstallString, and apt-get remove (sg-admind's
#      apt-remove) for a Linux app -- never one of the system's own packages
#   K2. David 2026-10-04: a click on an app opens its page -- where it comes
#      from, its whole description (winget's locale manifest; apt-cache for a
#      Linux app), site, licence, size; Back and Esc return to the list
#   M. David 2026-10-03: several apps at once, the administrator asked once:
#      --install-batch of two Linux apps starts one elevated helper, which
#      runs both apt installs and then goes; another process is refused by
#      it; in the window, check boxes and Install selected (N) install them
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
#   SG_MUTANT_STORE_ALWAYS_LIGHT  draws light in dark mode (David 2026-10-02: it switched only half)
#   SG_MUTANT_NOCUSTOM    drops a manifest's Custom switches (Opera stopped with 103)
#   SG_MUTANT_NOLAUNCH    Open shows Apps & features instead of starting the program
#   SG_MUTANT_NOELEVATE   ignores ElevationRequirement: elevationRequired
#   SG_MUTANT_NOWOWCU     misses per-user entries under HKCU\Software\WOW6432Node
#   SG_MUTANT_NOPLUS      refuses a winget id with + (Notepad++.Notepad++)
#   SG_MUTANT_NODESKTOP   runs a Linux app's desktop entry as a program
#   SG_MUTANT_NOUNINSTALL offers no Uninstall for a system package (SG Office)
#   SG_MUTANT_NOPAIR      lists both builds of an app as apps of their own
#   SG_MUTANT_BATCH_PER_APP  asks for an administrator per app in a batch
#   SG_MUTANT_HELPER_ANYONE  the batch's elevated helper runs anyone's request
#   SG_MUTANT_NOICON      keeps the letter badges
#   SG_MUTANT_NOQUEUE     ignores an Install while another runs (the old store)
#   SG_MUTANT_NOQUIET     runs the uninstaller's UI, not its QuietUninstallString
#   SG_MUTANT_NOCABDLL    cannot take a runtime out of a package's cabinet (H3)
#   SG_MUTANT_WHOLEHIT    a card cut by the window's bottom edge has no buttons to click (W2)
#   SG_MUTANT_NO_SELECT_ALL  Ctrl+A in the search box selects nothing (K2)
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

# Our own apps' Open runs what they install: a Linux one (SG Mail) its
# .desktop file, a Windows one a program this package installs -- SG Mail's
# entry named an sg-mail.exe nothing provides, and Open did nothing (David
# 2026-10-05).
bad=$(awk '/^\[/ { s = ""; r = "" } /^"Source"=/ { s = $0 } /^"Run"=/ { r = $0; if (s ~ /"ours:/) print r }' "$HERE/defaults/85-sg-store.reg" |
    sed 's/^"Run"="//; s/"$//' | while IFS= read -r run; do
        case "$run" in
            /usr/share/applications/*.desktop|/usr/bin/*) ;;
            /*) echo "$run" ;;
            *.exe) grep -q "build/${run%.exe}64.exe" "$HERE/debian/rules" || echo "$run" ;;
        esac
    done)
[ -z "$bad" ] && pass "our own apps' Open runs something they install" || fail "our apps' Run names nothing installed: $bad"

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

STORE_SRC="$HERE/src/store/main.c $HERE/src/store/details.c $HERE/src/store/catalog.c $HERE/src/store/sysinstall.c $HERE/src/store/icons.c $HERE/src/browser/fetch.c $HERE/src/browser/manifest.c $HERE/src/zip/zipcore.c"
STORE_LIBS="-lsetupapi -lwininet -lbcrypt -lshlwapi -lshell32 -lgdi32 -luser32 -ladvapi32 -lole32 -luuid -lwindowscodecs -lmsimg32 -lcomdlg32"
build_mut() { # define outfile
    # shellcheck disable=SC2086
    "$MINGW" -municode -mwindows -O1 -Wno-missing-field-initializers -I"$HERE/src/browser" -I"$HERE/src/store" -I"$HERE/src/zip" \
        "-D$1" -o "$2" $STORE_SRC $STORE_LIBS 2>>"$T/cc.log"
}
for m in NOHASH LINUXMIXED LINUXBYNAME SUBSTRING NOUPDATE ANYTYPE NOZIP NOSEARCH NOCUSTOM NOLAUNCH NOELEVATE NOWOWCU NOPLUS NODESKTOP NODEPINSTALL NOUNINSTALL NOPAIR NOICON NOQUEUE NOQUIET NOIEXPRESS NOCABDLL WHOLEHIT BATCH_PER_APP HELPER_ANYONE STORE_ALWAYS_LIGHT NO_SELECT_ALL; do
    build_mut "SG_MUTANT_$m" "$T/mut-$(echo $m | tr 'A-Z' 'a-z').exe" || fail "mutant $m does not build: $(tail -3 "$T/cc.log")"
done

# the stand-ins
"$MINGW" -municode -O1 -o "$T/store-fake.exe" "$HERE/test/sg-store-fake.c" -lole32 -luuid -lshell32 || { fail "the stand-ins do not build"; exit 1; }
"$MINGW" -municode -O1 -o "$T/store-height.exe" "$HERE/test/sg-store-height.c" -luser32 || { fail "the stand-ins do not build"; exit 1; }

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
# what Fake App says about itself (its locale manifest): the details page's text
cat > "$W/raw/f/Fake/App/1.10.0/Fake.App.locale.en-US.yaml" <<'EOF'
PackageIdentifier: Fake.App
PackageVersion: 1.10.0
PackageLocale: en-US
Publisher: Fake Makers
PublisherUrl: https://fake.example
PackageName: Fake App
PackageUrl: https://fake.example/app
License: MIT
ShortDescription: A fake app for the gate.
Description: |-
  Fake App's first paragraph, the long one,
  over two lines.

  Its second paragraph: everything it does.
ManifestType: defaultLocale
ManifestVersion: 1.6.0
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
# -s: what would go (only the package asked for); remove: out of dpkg's
# status and the installed list
case " \$* " in *" -s "*) case " \$* " in *" remove "*) echo "Remv \$last [9.9-gate]" ;; esac; exit 0 ;; esac
case " \$* " in *" remove "*)
    awk -v p="Package: \$last" 'BEGIN { RS = ""; ORS = "\\n\\n" } index(\$0 "\\n", p "\\n") != 1' "$T/dpkg-status" > "$T/dpkg-status.n"
    mv "$T/dpkg-status.n" "$T/dpkg-status"; grep -v "^\$last " "$T/installed" > "$T/installed.n"; mv "$T/installed.n" "$T/installed"
    [ -n "\$fd" ] && eval "printf 'pmstatus:x:50:Removing the gate package\n' >&\$fd"
    exit 0 ;;
esac
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
export SG_STORE_DIRECT=1 SG_DEBINFO="$HERE/src/store/sg-debinfo" SG_APPINFO="$HERE/src/store/sg-appinfo" SG_APTCACHE="$B/apt-cache"
# the package lists, as apt-cache reads them, for the details page
cat > "$B/apt-cache" <<'EOF'
#!/bin/sh
case "$1 $*" in
show*sg-gate-tool*) printf 'Package: sg-gate-tool\nVersion: 2.5-1\nInstalled-Size: 2048\nMaintainer: Gate Team <gate@example.org>\nSection: utils\nHomepage: https://gate.example.org\nDescription-en: a tool for the gate\n The gate tool long description, first paragraph.\n .\n Second paragraph of the gate tool.\nDescription-md5: 0\n' ;;
policy*sg-gate-tool*) printf 'sg-gate-tool:\n  Installed: (none)\n  Candidate: 2.5-1\n  Version table:\n     2.5-1 500\n        500 http://deb.debian.org/debian trixie/main amd64 Packages\n' ;;
esac
EOF
chmod 755 "$B/apt-cache"
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
# A Linux app whose Run is its desktop entry: gio launch starts it (the
# Store's newer Linux apps name their .desktop, not a guessed program path)
if [ -x /usr/bin/gio ]; then
    printf '[Desktop Entry]\nType=Application\nName=Gate Linux\nExec=sh -c "echo linux-desktop >> %s/launch.log"\n' "$G" \
        > "$T/gate-linux.desktop"
    app L3 'Gate Linux' Utilities linux 'linux:apt:sg-gate-linux'
    reg "$K\\L3" /v Run /d "$T/gate-linux.desktop"
    rm -f "$G/launch.log"
    run --open L3
    launched 'linux-desktop' && pass "Open starts a Linux app by its desktop entry (gio launch)" \
        || fail "Open L3 (desktop entry): $(cat "$G/launch.log" 2>/dev/null)"
    rm -f "$G/launch.log"
    wine "$T/mut-nodesktop.exe" --open L3 >/dev/null 2>&1; sleep 3
    ! grep -q 'linux-desktop' "$G/launch.log" 2>/dev/null \
        && pass "MUTANT NODESKTOP runs the .desktop file as a program (gate catches it)" || fail "NODESKTOP not detected"
else
    echo "SKIP  Open by desktop entry (no /usr/bin/gio)"
fi
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

# --- G2. a package's dependencies first (TortoiseGit needs Git) ----------------------------------
mkdir -p "$W/api/f/Fake" "$W/raw/f/Fake/Dep/1.0" "$W/raw/f/Fake/Needy/1.0"
printf '[{"name": "1.0", "type": "dir"}]' > "$W/api/f/Fake/Dep"
printf '[{"name": "1.0", "type": "dir"}]' > "$W/api/f/Fake/Needy"
cat > "$W/raw/f/Fake/Dep/1.0/Fake.Dep.installer.yaml" <<EOF
PackageIdentifier: Fake.Dep
PackageVersion: 1.0
InstallerType: nullsoft
Installers:
- Architecture: x64
  InstallerSwitches:
    Silent: /S /dep-installed
  InstallerUrl: http://127.0.0.1:$PORT/files/store-fake.exe
  InstallerSha256: $SHA
EOF
cat > "$W/raw/f/Fake/Needy/1.0/Fake.Needy.installer.yaml" <<EOF
PackageIdentifier: Fake.Needy
PackageVersion: 1.0
InstallerType: nullsoft
Installers:
- Architecture: x64
  InstallerSwitches:
    Silent: /S /needy
  InstallerUrl: http://127.0.0.1:$PORT/files/store-fake.exe
  InstallerSha256: $SHA
  Dependencies:
    PackageDependencies:
    - PackageIdentifier: Fake.Dep
      MinimumVersion: 1.0
    - PackageIdentifier: Microsoft.VCRedist.2015+.x64
EOF
app 10 'Needy App' Development windows 'winget:Fake.Needy' 'Needy App'
wineserver -w
needy() { SG_FAKE_KEY=NeedyApp SG_FAKE_DISPLAY='Needy App' SG_FAKE_VERSION=1.0 SG_FAKE_DIR='Needy App' \
              wine "${SG_MUT:-$EXE}" --install 10 >/dev/null 2>&1; }
rm -f "$G/setup.log"; hl=$(wc -l < "$T/http.log")
needy; ec=$?
[ "$ec" = 0 ] && pass "--install of a package with dependencies succeeds" || fail "install 10 exit $ec: $(cat "$D.result" 2>/dev/null)"
[ "$(grep -n -- '/dep-installed' "$G/setup.log" 2>/dev/null | head -1 | cut -d: -f1)" = 1 ] && grep -q -- '/needy' "$G/setup.log" \
    && pass "its dependency is installed first, then the package" || fail "setup.log: $(tr '\n' '|' < "$G/setup.log" 2>/dev/null)"
tail -n +"$((hl + 1))" "$T/http.log" | grep -qi vcredist && fail "the Visual C++ runtime was fetched" \
    || pass "the Visual C++ runtime, which the system provides, is not fetched"
needy
[ "$(grep -c -- '/dep-installed' "$G/setup.log" 2>/dev/null)" = 1 ] && [ "$(grep -c -- '/needy' "$G/setup.log")" = 2 ] \
    && pass "installed again, the dependency is not installed twice" || fail "setup.log: $(tr '\n' '|' < "$G/setup.log" 2>/dev/null)"
wine reg delete 'HKLM\Software\Stained Glass\Store\Dependencies' /f >/dev/null 2>&1
wine reg delete 'HKCU\Software\Stained Glass\Store\Dependencies' /f >/dev/null 2>&1
rm -f "$G/setup.log"
SG_MUT="$T/mut-nodepinstall.exe" needy
! grep -q -- '/dep-installed' "$G/setup.log" 2>/dev/null && pass "MUTANT NODEPINSTALL skips the dependency (gate catches it)" \
    || fail "NODEPINSTALL not detected"
wine reg delete "$K\\10" /f >/dev/null 2>&1; ununinstall NeedyApp; rm -f "$G/setup.log"; wineserver -w

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

# --- H2. an IExpress package of MSIs (SQL Server Compact's runtime) -------------------------------
# Two MSIs, x86 then x64, under one consent; the "x64" one may only install
# after the x86 one (as SQL Server Compact's x64 MSI needs its x86 one).
# (The desktop is up: wineserver -w would wait for it forever.)
if command -v wixl >/dev/null; then
    M="$WINEPREFIX/drive_c/gate/msis"; mkdir -p "$M"
    gate_msi() { # file name condition
        cat > "$T/$1.wxs" <<EOF2
<?xml version="1.0"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="*" Name="$2" Language="1033" Version="1.0.0" Manufacturer="The gate" UpgradeCode="$(python3 -c 'import uuid; print(uuid.uuid4())')">
    <Package InstallerVersion="200" Compressed="yes" InstallScope="perMachine"/>
    <Media Id="1" Cabinet="g.cab" EmbedCab="yes"/>
    $3
    <Directory Id="TARGETDIR" Name="SourceDir">
      <Component Id="C" Guid="$(python3 -c 'import uuid; print(uuid.uuid4())')">
        <RegistryValue Root="HKLM" Key="Software\SgGateCompact" Name="$1" Type="string" Value="1" KeyPath="yes"/>
      </Component>
    </Directory>
    <Feature Id="F" Level="1"><ComponentRef Id="C"/></Feature>
  </Product>
</Wix>
EOF2
        wixl -o "$M/$1.msi" "$T/$1.wxs" 2>>"$T/cc.log"; }
    gate_msi compact_x86 'Gate Compact x86' ''
    gate_msi compact_x64 'Gate Compact x64' '<Property Id="X86"><RegistrySearch Id="S" Root="HKLM" Key="Software\SgGateCompact" Name="compact_x86" Type="raw"/></Property><Condition Message="the x86 one first">X86</Condition>'
    reg "$K\\10" /v Name /d 'Gate Compact'; reg "$K\\10" /v Tier /d windows
    reg "$K\\10" /v Source /d "pin:http://127.0.0.1:$PORT/files/store-fake.exe|$SHA|iexpress-msi|"
    reg "$K\\10" /v DetectName /d 'Gate Compact'
    sleep 1
    compact() { for v in x86 x64; do { wine reg query 'HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall' /s /reg:32
                wine reg query 'HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall' /s /reg:64; } 2>/dev/null \
                | grep -q "Gate Compact $v" || return 1; done; }
    rm -f "$G/setup.log"
    SG_FAKE_MSIS='C:\gate\msis' wine "$EXE" --install 10 >/dev/null 2>&1; ec=$?
    grep -q '/Q /T:.* /C' "$G/setup.log" 2>/dev/null && pass "an IExpress package is unpacked (/Q /T: /C)" || fail "setup.log '$(cat "$G/setup.log" 2>/dev/null)'"
    [ "$ec" = 0 ] && compact && pass "and its MSIs install, x86 first (the x64 one needs it)" \
        || fail "install 10 exit $ec: $(cat "$D.result" 2>/dev/null)"
    grep -q '^elevated 10 1' "$D.result" 2>/dev/null && pass "as an administrator" || fail "10 elevated: $(cat "$D.result" 2>/dev/null)"
    for v in x64 x86; do wine msiexec /x "C:\\gate\\msis\\compact_$v.msi" /quiet >/dev/null 2>&1; done
    sleep 1
    SG_FAKE_MSIS='C:\gate\msis' wine "$T/mut-noiexpress.exe" --install 10 >/dev/null 2>&1
    ! compact && pass "MUTANT NOIEXPRESS installs neither (gate catches it)" || fail "NOIEXPRESS not detected"
    wine reg delete "$K\\10" /f >/dev/null 2>&1; sleep 1
else
    echo "NOTE  no wixl (msitools): the IExpress package install is not checked"
fi

# --- H3. a runtime out of a self-extracting package's cabinet (the Visual Basic 6 runtime) -------
# David 2026-10-05: Meedio's plug-ins need MSVBVM60.DLL, which Microsoft now
# offers only inside Windows XP SP3's package. Its stand-in: a program stub
# (with a decoy "MSCF") and a cabinet holding i386\gatert.dl_, a one-file
# cabinet of a 32-bit DLL whose DllRegisterServer marks the registry. The
# DLL lands in the 32-bit system folder, registered, with an Uninstall entry
# the store detects -- as an administrator, once; its page shows the
# catalogue's licence and no Open (nothing to open); Uninstall removes it.
if command -v i686-w64-mingw32-gcc >/dev/null; then
    cat > "$T/gatert.c" <<'EOF2'
#include <windows.h>
__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\SgGateRuntime", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return E_FAIL;
    RegSetValueExW(k, L"Registered", 0, REG_SZ, (const BYTE *)L"1", 4);
    RegCloseKey(k);
    return S_OK;
}
EOF2
    i686-w64-mingw32-gcc -shared -O2 -Wl,--kill-at -o "$T/gatert.dll" "$T/gatert.c" 2>>"$T/cc.log" \
        && python3 "$HERE/test/store-cab.py" "$T/inner.cab" "gatert.dll=$T/gatert.dll" \
        && python3 "$HERE/test/store-cab.py" "$T/outer.cab" "i386\\other.dl_=$T/gatert.c" "i386\\gatert.dl_=$T/inner.cab" \
        && { head -c 3000 /dev/zero; printf 'MSCF\001\002\003\004'; head -c 999 /dev/zero; cat "$T/outer.cab"; } > "$W/files/gate-sp.exe" \
        || fail "the runtime's stand-in package does not build: $(tail -3 "$T/cc.log")"
    RSHA=$(sha256sum "$W/files/gate-sp.exe" | cut -d' ' -f1)
    reg "$K\\11" /v Name /d 'Gate Runtime'; reg "$K\\11" /v Tier /d windows; reg "$K\\11" /v Category /d Development
    reg "$K\\11" /v Source /d "pin:http://127.0.0.1:$PORT/files/gate-sp.exe|$RSHA|cab-dll|i386\\gatert.dl_|gatert.dll|Gate Runtime|1.2.3"
    reg "$K\\11" /v PinVersion /d 1.2.3; reg "$K\\11" /v DetectName /d 'Gate Runtime'
    reg "$K\\11" /v License /d 'Proprietary: the gate maker'"'"'s licence'
    reg "$K\\11" /v Homepage /d 'https://gate.example/runtime'
    sleep 1
    RT="$WINEPREFIX/drive_c/windows/syswow64/gatert.dll"
    rt_reg() { wine reg query 'HKLM\Software\SgGateRuntime' /v Registered /reg:32 2>/dev/null | grep -q 'REG_SZ *1'; }
    wine "$EXE" --install 11 >/dev/null 2>&1; ec=$?
    [ "$ec" = 0 ] && cmp -s "$RT" "$T/gatert.dll" && pass "a runtime is taken out of the package's cabinet (and its .dl_) into the 32-bit system folder" \
        || fail "install 11 exit $ec: $(cat "$D.result" 2>/dev/null); $(ls -la "$RT" 2>&1)"
    rt_reg && pass "...and registered (its 32-bit DllRegisterServer ran)" || fail "gatert.dll not registered"
    grep -q '^elevated 11 1' "$D.result" 2>/dev/null && pass "...as an administrator" || fail "11 elevated: $(cat "$D.result" 2>/dev/null)"
    run --list
    [ "$(kv "$(appline 11)" state)" = installed ] && [ "$(kv "$(appline 11)" installed)" = 1.2.3 ] \
        && pass "the store sees it installed (its Uninstall entry), version 1.2.3" || fail "after install: $(appline 11)"
    # its page, as sg-notify's question opens it
    rm -f "$D"
    wine "$EXE" --page 11 >/dev/null 2>&1 &
    if waitfor "$D" '^detail 11 ready=1' 60; then
        sleep 1
        [ "$(field detail-license)" = "Proprietary: the gate maker's licence" ] && [ "$(field detail-homepage)" = https://gate.example/runtime ] \
            && pass "--page opens its page: its licence and site (the catalogue's)" || fail "page facts: $(field detail-license) | $(field detail-homepage)"
        grep -q '^hit open 11 ' "$D" && fail "a runtime offers Open (nothing to open)" || pass "...and no Open: a runtime has nothing to open"
        grep -q '^hit uninstall 11 ' "$D" && pass "...but Uninstall" || fail "no Uninstall on the runtime's page"
        import -window root "$OUT/store-runtime.png" 2>/dev/null
    else fail "--page 11 did not open its page: $(grep '^detail' "$D" 2>/dev/null | head -2 | tr '\n' '|')"; fi
    wine taskkill /f /im sg-store64.exe >/dev/null 2>&1; sleep 0.5
    rm -f "$D.result"
    wine "$EXE" --uninstall 11 >/dev/null 2>&1
    [ ! -e "$RT" ] && ! { wine reg query 'HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\SG-gatert.dll' /reg:32 >/dev/null 2>&1; } \
        && pass "Uninstall removes it and its entry" || fail "uninstall 11: $(cat "$D.result" 2>/dev/null); $(ls "$RT" 2>&1)"
    wine reg delete 'HKLM\Software\SgGateRuntime' /f /reg:32 >/dev/null 2>&1
    wine "$T/mut-nocabdll.exe" --install 11 >/dev/null 2>&1
    [ ! -e "$RT" ] && pass "MUTANT NOCABDLL installs nothing (gate catches it)" || fail "NOCABDLL not detected"
    wine reg delete "$K\\11" /f >/dev/null 2>&1; sleep 1
else
    echo "NOTE  no i686-w64-mingw32-gcc: the runtime (cab-dll) install is not checked"
fi

# --- I. a Linux app, through the consent (direct here) and sg-admind ------------------------------
rm -f "$D" "$D.result" "$D.sys"
wine "$EXE" --install L2 >/dev/null 2>&1 & IP=$!
if waitfor "$D.sys" '^result ' 120; then
    grep -q '^result ok' "$D.sys" && pass "the elevated copy's request is answered: installed" || fail "result: $(grep '^result' "$D.sys")"
    grep -q '^progress [0-9]* .*gate package' "$D.sys" && pass "apt's progress reaches the progress window" || fail "no progress: $(cat "$D.sys")"
    import -window root "$OUT/store-apt-progress.png" 2>/dev/null
    grep -q 'install sg-gate-tool' "$T/apt.log" && pass "sg-admind ran apt-get install sg-gate-tool" || fail "apt.log: $(cat "$T/apt.log")"
else fail "the elevated install did not finish: $(cat "$D.sys" 2>/dev/null) / $(tail -3 "$T/admind.log")"; fi
# installed, the progress window goes by itself (no Close click: the Store
# walk found it waiting for one), and the store's --install with it
i=0; while pgrep -f -- '[-]-elevated-apt.*sg-gate-tool' >/dev/null && [ $i -lt 24 ]; do sleep 0.25; i=$((i + 1)); done
pgrep -f -- '[-]-elevated-apt.*sg-gate-tool' >/dev/null && left=1 || left=0
[ $left = 0 ] && pass "installed, the progress window closes by itself" || fail "the progress window waited for a click"
[ $left = 0 ] || click "$D.sys" close
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
# --- I3. Uninstall: our suite, through the consent and sg-admind's apt-remove ----------------------
rm -f "$D" "$D.result" "$D.sys"; : > "$T/apt.log"
wine "$EXE" --uninstall O5 >/dev/null 2>&1 & IP=$!
if waitfor "$D.sys" '^result ' 120; then
    grep -q '^result ok' "$D.sys" && pass "Uninstall goes through the elevated copy and sg-admind's apt-remove" || fail "O5 uninstall: $(grep '^result' "$D.sys")"
else fail "the uninstall did not finish: $(cat "$D.sys" 2>/dev/null) / $(tail -3 "$T/admind.log")"; fi
i=0; while kill -0 $IP 2>/dev/null && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
kill $IP 2>/dev/null; wait $IP 2>/dev/null
grep -q '^uninstall O5 ok' "$D.result" 2>/dev/null && pass "the store's --uninstall succeeds" || fail "uninstall O5: $(cat "$D.result" 2>/dev/null)"
# (taking its registrations out of the Windows side: test/admind-check.sh)
grep -q -- '^-y .* remove sg-office$' "$T/apt.log" && pass "sg-admind ran apt-get remove sg-office" || fail "apt.log: $(cat "$T/apt.log")"
run --list
[ "$(kv "$(appline O5)" state)" = not-installed ] && pass "and it is then not installed (dpkg's status)" || fail "O5 after uninstall: $(appline O5)"
# the mutant offers no Uninstall: the Linux app stays
rm -f "$D.result"; : > "$T/apt.log"
wine "$T/mut-nouninstall.exe" --uninstall L2 >/dev/null 2>&1
grep -q '^uninstall L2 fail' "$D.result" 2>/dev/null && ! grep -q ' remove ' "$T/apt.log" \
    && pass "MUTANT NOUNINSTALL cannot uninstall (gate catches it)" || fail "NOUNINSTALL not detected: $(cat "$D.result" 2>/dev/null)"
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
    # maximized by a double-click on its title bar (Win+Up depends on the
    # desktop's hotkeys having the keyboard, which a gate cannot count on)
    xdotool mousemove 300 16 click --repeat 2 --delay 120 1; sleep 1.5
    xdotool key Down; sleep 0.6
    [ "$(field columns)" = 2 ] && pass "a wide window shows the cards in two columns" || fail "columns '$(field columns)' at full width"
    import -window root "$OUT/store-wide.png" 2>/dev/null
else fail "the window did not open"; fi
wine taskkill /f /im sg-store64.exe >/dev/null 2>&1; sleep 0.5
v=$(window_checks "$T/mut-nosearch.exe")
[ "$v" != NOWINDOW ] && [ "$v" != "2 05 L1" ] && pass "MUTANT NOSEARCH lists everything after \"gimp\" (gate catches it)" || fail "NOSEARCH not detected ('$v')"
wine taskkill /f /im mut-nosearch.exe >/dev/null 2>&1

# --- K3. dark mode: what it draws is dark too, not only its search box -------------------------
dark_check() { # exe -- "MODE LUMA": the dump's mode and the window's background's brightness
    rm -f "$D"
    wine reg add 'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' /v AppsUseLightTheme /t REG_DWORD /d 0 /f >/dev/null 2>&1
    wine "$1" >/dev/null 2>&1 &
    waitfor "$D" '^window 1' 60 || { echo "NOWINDOW"; return; }
    sleep 1.2
    w=$(xdotool search --name 'SG Store' 2>/dev/null | tail -1)
    eval "$(xdotool getwindowgeometry --shell "$w" 2>/dev/null)"
    # the background, left of the cards near the window's bottom
    l=$(import -window root -crop "1x1+$((X + 6))+$((Y + HEIGHT - 8))" -colorspace gray -format '%[fx:int(255*u)]' info: 2>/dev/null)
    echo "$(field mode) ${l:-?}"
    wine reg add 'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' /v AppsUseLightTheme /t REG_DWORD /d 1 /f >/dev/null 2>&1
}
v=$(dark_check "$EXE")
case "$v" in "dark "[0-9]*) [ "${v#dark }" -lt 80 ] && pass "in dark mode the Store draws dark (mode dark, background $v)" || fail "dark mode, light background: $v";;
    *) fail "dark mode: '$v'";; esac
import -window root "$OUT/store-dark.png" 2>/dev/null
wine taskkill /f /im sg-store64.exe >/dev/null 2>&1; sleep 0.5
v=$(dark_check "$T/mut-store_always_light.exe")
case "$v" in "light "*|*" "2[0-9][0-9]) pass "MUTANT STORE_ALWAYS_LIGHT draws light in dark mode (gate catches it: $v)";; *) fail "STORE_ALWAYS_LIGHT not detected ('$v')";; esac
wine taskkill /f /im mut-store_always_light.exe >/dev/null 2>&1; sleep 0.5

# --- K2. an app's page (David 2026-10-04): a click on it, not its buttons, says more -------------
# where it comes from, the whole description, its site and licence; Back
rm -f "$D"
wine "$EXE" >/dev/null 2>&1 &
if waitfor "$D" '^window 1' 60; then
    sleep 0.8
    click "$D" "search -"; sleep 0.4; xdotool type --delay 60 fake; sleep 1
    click "$D" "card 01" && waitfor "$D" '^detail 01 ready=1' 60
    if grep -q '^detail 01 ready=1' "$D"; then
        pass "a click on Fake App opens its page"
        grep -q "^detail-desc Fake App's first paragraph, the long one,|over two lines.||Its second paragraph: everything it does.$" "$D" \
            && pass "...with its whole description, from the winget repository's locale manifest" || fail "description: $(field detail-desc)"
        grep -q '^detail-source .*winget community repository lists it (Fake.App).*SHA-256' "$D" && pass "...where it comes from (its maker's site, as winget lists it, checked)" \
            || fail "source: $(field detail-source)"
        [ "$(field detail-homepage)" = https://fake.example/app ] && [ "$(field detail-license)" = MIT ] && [ "$(field detail-version)" = 1.10.0 ] \
            && pass "...its site, licence and latest version" || fail "facts: $(field detail-homepage) $(field detail-license) $(field detail-version)"
        grep -q '^hit install 01 ' "$D" && pass "...and its Install button" || fail "no Install on the page"
        import -window root "$OUT/store-details.png" 2>/dev/null
        click "$D" "back -"; sleep 1
        [ "$(field detail)" = - ] && [ "$(field search)" = fake ] && pass "Back returns to the list as it was (the search kept)" || fail "after Back: detail '$(field detail)' search '$(field search)'"
        # Ctrl+A in the search box selects it: typing replaces the search
        click "$D" "search -"; sleep 0.4; xdotool key ctrl+a; sleep 0.3; xdotool type --delay 60 gim; sleep 1
        [ "$(field search)" = gim ] && pass "Ctrl+A in the search box selects it: typing replaces the search" || fail "Ctrl+A then typing: search '$(field search)'"
        xdotool key ctrl+a; sleep 0.3; xdotool type --delay 60 fake; sleep 1
    else fail "a click on the card did not open its page: $(grep '^detail' "$D" | head -3 | tr '\n' '|')"; fi
    xdotool key Escape; sleep 0.4; xdotool key Escape; sleep 0.6     # to the search box, then cleared
    click "$D" "category Linux" && sleep 0.8
    click "$D" "card L2" && waitfor "$D" '^detail L2 ready=1' 60
    grep -q '^detail-desc The gate tool long description, first paragraph.||Second paragraph of the gate tool.$' "$D" \
        && pass "a Linux app's page has the package's long description (apt-cache)" || fail "Linux description: $(field detail-desc)"
    grep -q "^detail-source .*package sg-gate-tool from deb.debian.org (Debian's own repository)" "$D" \
        && pass "...and the repository apt gets it from" || fail "Linux source: $(field detail-source)"
    [ "$(field detail-size)" = "2.0 MB" ] && pass "...and its installed size" || fail "size: $(field detail-size)"
    xdotool key Escape; sleep 0.8
    [ "$(field detail)" = - ] && pass "Esc closes the page" || fail "Esc: detail '$(field detail)'"
else fail "the window did not open (details)"; fi
wine taskkill /f /im sg-store64.exe >/dev/null 2>&1; sleep 0.5
# mutant: Ctrl+A selects nothing -- the typing is added to the search
rm -f "$D"
wine "$T/mut-no_select_all.exe" >/dev/null 2>&1 &
if waitfor "$D" '^window 1' 60; then
    sleep 0.8
    click "$D" "search -"; sleep 0.4; xdotool type --delay 60 fake; sleep 1; xdotool key ctrl+a; sleep 0.3; xdotool type --delay 60 gim; sleep 1
    [ "$(field search)" != gim ] && pass "MUTANT NO_SELECT_ALL caught (search '$(field search)')" || fail "MUTANT NO_SELECT_ALL not caught"
else fail "the mutant's window did not open"; fi
wine taskkill /f /im mut-no_select_all.exe >/dev/null 2>&1; sleep 0.5

# --- L. one app, two builds; its picture; the queue; Uninstall -----------------------------------
reg "$K\\05" /v Linux /d L1
sleep 0.5
run --list
grep -q '^pair 05 L1 choice=linux target=L1$' "$D" && pass "GIMP's Windows and Linux builds are one app, suggesting the Linux build" \
    || fail "pair: $(grep '^pair' "$D")"
case " $(view) " in *" L1 "*) fail "the Linux GIMP is still listed on its own: $(view)" ;;
    *) pass "the Linux GIMP is no card of its own: no app is listed twice ($(view))" ;; esac
SG_MUT="$T/mut-nopair.exe" run --list
case " $(view) " in *" 05 "*" L1 "*) pass "MUTANT NOPAIR lists GIMP twice (gate catches it)" ;; *) fail "NOPAIR not detected: $(view)" ;; esac
reg "$K\\05" /v Prefer /d windows
run --list
grep -q '^pair 05 L1 choice=windows target=05$' "$D" && pass "Prefer=windows suggests the Windows build (a browser and its sign-on)" \
    || fail "Prefer=windows: $(grep '^pair' "$D")"
wine reg delete "$K\\05" /v Prefer /f >/dev/null 2>&1
uninstall GIMP_is1 'GIMP' 3.0
sleep 0.5
run --list
grep -q '^pair 05 L1 choice=linux target=05$' "$D" && pass "the build that is installed is the card's (Open, Uninstall)" \
    || fail "installed build: $(grep '^pair' "$D")"
ununinstall GIMP_is1; sleep 0.5

# pictures: a PNG and an ICO from the web, drawn on the cards, cached
convert -size 64x64 xc:'#ff0000' "PNG32:$W/files/icon.png" 2>/dev/null
convert -size 32x32 xc:'#00ff00' "$W/files/icon.ico" 2>/dev/null
app 08 'Pick App' Utilities windows 'winget:Fake.Pick' 'Pick App'
reg "$K\\01" /v Icon /d "http://127.0.0.1:$PORT/files/icon.png"
reg "$K\\08" /v Icon /d "http://127.0.0.1:$PORT/files/icon.ico"
reg "$K\\01" /v Description /d 'Fake App, a queue-test app.'
reg "$K\\08" /v Description /d 'Pick App, a queue-test app.'
sleep 0.5
export SG_STORE_ICON_CACHE='C:\gate\icons'
store_window() { # exe -- opens it on "queue-test"; FALSE if it did not open
    rm -f "$D"
    SG_STORE_YES=1 SG_FAKE_KEY=FakeApp SG_FAKE_DISPLAY='Fake App' SG_FAKE_VERSION=1.10.0 SG_FAKE_DIR='Fake App' SG_FAKE_SLEEP=5000 \
        wine "$1" --search queue-test >/dev/null 2>&1 &
    waitfor "$D" '^window 1' 60 || return 1
    waitfor "$D" '^icons 2' 60
    sleep 1
}
pixel() { # dump verb ord -> the screen's colour at that hit, as #RRGGBB
    xy=$(grep "^hit $2 $3 " "$1" 2>/dev/null | tail -1 | awk '{ print $(NF-1), $NF }'); [ -n "$xy" ] || { echo none; return; }
    import -window root -crop "1x1+${xy% *}+${xy#* }" -depth 8 txt:- 2>/dev/null | grep -o '#[0-9A-Fa-f]\{6\}' | head -1
}
if store_window "$EXE"; then
    [ "$(field icons)" = 2 ] && pass "both pictures (PNG, ICO) are fetched" || fail "icons '$(field icons)'"
    [ "$(pixel "$D" icon 01 | tr a-f A-F)" = '#FF0000' ] && pass "the card shows its picture, not a letter" || fail "01's picture: $(pixel "$D" icon 01)"
    [ "$(pixel "$D" icon 08 | tr a-f A-F)" = '#00FF00' ] && pass "an ICO's too" || fail "08's picture: $(pixel "$D" icon 08)"
    [ "$(ls "$G/icons" 2>/dev/null | wc -l)" = 2 ] && pass "and they are cached for next time" || fail "cache: $(ls "$G/icons" 2>/dev/null)"
    import -window root "$OUT/store-icons.png" 2>/dev/null
    rm -f "$G/setup.log"
    click "$D" "install 01"; sleep 0.8; click "$D" "install 08"; sleep 1
    grep -q '^queued 08 install' "$D" && pass "a second Install while one runs waits its turn" || fail "not queued: $(grep -E '^(queued|app 0[18])' "$D")"
    import -window root "$OUT/store-queue.png" 2>/dev/null
    i=0; while ! { grep -q '^app 08 .*state=\(done\|installed\)' "$D" && grep -q '^app 01 .*state=\(done\|installed\)' "$D"; } && [ $i -lt 120 ]; do sleep 0.5; i=$((i + 1)); done
    [ "$(grep -c -- '/gate-silent\|/picked-nullsoft' "$G/setup.log" 2>/dev/null)" = 2 ] && head -1 "$G/setup.log" | grep -q -- '/gate-silent' \
        && pass "then installs: one after the other, in the order asked" || fail "setup.log: $(tr '\n' '|' < "$G/setup.log" 2>/dev/null)"
    rm -f "$G/uninstall.log"
    waitfor "$D" '^hit uninstall 01 ' 20; click "$D" "uninstall 01"
    i=0; while ! grep -q '^app 01 .*state=not-installed' "$D" && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
    grep -q '/quiet' "$G/uninstall.log" 2>/dev/null && grep -q '^app 01 .*state=not-installed' "$D" \
        && pass "Uninstall runs its QuietUninstallString, and the app is gone" || fail "uninstall: $(cat "$G/uninstall.log" 2>/dev/null) / $(appline 01)"
else fail "the window (pictures, queue) did not open"; fi
wine taskkill /f /im sg-store64.exe >/dev/null 2>&1; sleep 0.5
ununinstall FakeApp; ununinstall PickApp; sleep 0.5
if store_window "$T/mut-noicon.exe"; then
    [ "$(pixel "$D" icon 01)" = none ] && pass "MUTANT NOICON draws letters (gate catches it)" || fail "NOICON not detected"
else fail "the NOICON window did not open"; fi
wine taskkill /f /im mut-noicon.exe >/dev/null 2>&1; sleep 0.5
if store_window "$T/mut-noqueue.exe"; then
    rm -f "$G/setup.log"
    click "$D" "install 01"; sleep 0.8; click "$D" "install 08"; sleep 8
    ! grep -q -- '/picked-nullsoft' "$G/setup.log" 2>/dev/null && pass "MUTANT NOQUEUE drops the second Install (gate catches it)" \
        || fail "NOQUEUE not detected"
else fail "the NOQUEUE window did not open"; fi
wine taskkill /f /im mut-noqueue.exe >/dev/null 2>&1; sleep 0.5
ununinstall FakeApp; ununinstall PickApp; sleep 0.5
# headless, and the uninstaller's UI for the mutant
SG_FAKE_KEY=FakeApp SG_FAKE_DISPLAY='Fake App' SG_FAKE_VERSION=1.10.0 SG_FAKE_DIR='Fake App' wine "$EXE" --install 01 >/dev/null 2>&1
rm -f "$G/uninstall.log"
SG_MUT="$T/mut-noquiet.exe" run --uninstall 01
grep -q '/ui' "$G/uninstall.log" 2>/dev/null && ! grep -q '/quiet' "$G/uninstall.log" && pass "MUTANT NOQUIET runs the uninstaller's UI (gate catches it)" \
    || fail "NOQUIET not detected: $(cat "$G/uninstall.log" 2>/dev/null)"
ununinstall FakeApp; sleep 0.5

# --- M. several apps, one consent ------------------------------------------------------------------
"$MINGW" -municode -O1 -o "$T/helperprobe.exe" "$HERE/test/sg-store-helperprobe.c" || fail "the helper probe does not build"
app L3 'Gate Two' Utilities linux 'linux:apt:sg-gate-two'
app L4 'Gate Three' Utilities linux 'linux:apt:sg-gate-three'
sleep 1   # (the desktop is up: wineserver -w would wait for it forever)
batch() { # exe -- installs L3 and L4 as one batch; the probe asks its helper too
    rm -f "$D" "$D.result" "$D.helper" "$G/pwned"; : > "$T/apt.log"
    wine "$1" --install-batch L3 L4 >/dev/null 2>&1 & BP=$!
    waitfor "$D.helper" '^helper ' 120
    hp=$(sed -n 's/^helper [0-9]* //p' "$D.helper" | head -1 | tr -d '\r')
    [ -n "$hp" ] && wine "$T/helperprobe.exe" "$hp" 'C:\windows\system32\cmd.exe' '/c echo x > C:\gate\pwned' > "$T/probe.out" 2>&1
    i=0; while kill -0 $BP 2>/dev/null && [ $i -lt 240 ]; do sleep 0.5; i=$((i + 1)); done
    kill $BP 2>/dev/null; wait $BP 2>/dev/null
}
batch "$EXE"
grep -q '^result L3 ok' "$D.result" 2>/dev/null && grep -q '^result L4 ok' "$D.result" 2>/dev/null \
    && grep -q 'install sg-gate-two' "$T/apt.log" && grep -q 'install sg-gate-three' "$T/apt.log" \
    && pass "a batch of two Linux apps: both installed (apt-get install each)" || fail "batch: $(cat "$D.result" 2>/dev/null) / $(cat "$T/apt.log")"
[ "$(grep -c '^helper ' "$D.helper" 2>/dev/null)" = 1 ] && pass "under one consent: one elevated helper for both" \
    || fail "consents in the batch: $(grep -c '^helper ' "$D.helper" 2>/dev/null)"
grep -q 'reply !refused' "$T/probe.out" && [ ! -e "$G/pwned" ] && pass "the helper refuses another process (it ran nothing for it)" \
    || fail "the helper answered another process: $(cat "$T/probe.out") $(ls "$G/pwned" 2>/dev/null)"
sleep 1; pgrep -f -- '[-]-elevated-helper' >/dev/null && fail "the helper stayed after the batch" || pass "and the helper goes when the batch is done"
# mutants: a consent per app; a helper that runs anyone's request
wine reg add 'HKLM\Software\Stained Glass\Store' /f >/dev/null 2>&1
for n in two three; do awk -v p="Package: sg-gate-$n" 'BEGIN { RS = ""; ORS = "\n\n" } index($0 "\n", p "\n") != 1' "$T/dpkg-status" > "$T/dpkg-status.n"; mv "$T/dpkg-status.n" "$T/dpkg-status"; grep -v "^sg-gate-$n " "$T/installed" > "$T/installed.n"; mv "$T/installed.n" "$T/installed"; done
batch "$T/mut-batch_per_app.exe"
[ "$(grep -c '^helper ' "$D.helper" 2>/dev/null || echo 0)" != 1 ] && pass "MUTANT BATCH_PER_APP: no one helper (gate catches it)" || fail "BATCH_PER_APP not detected"
for n in two three; do awk -v p="Package: sg-gate-$n" 'BEGIN { RS = ""; ORS = "\n\n" } index($0 "\n", p "\n") != 1' "$T/dpkg-status" > "$T/dpkg-status.n"; mv "$T/dpkg-status.n" "$T/dpkg-status"; grep -v "^sg-gate-$n " "$T/installed" > "$T/installed.n"; mv "$T/installed.n" "$T/installed"; done
batch "$T/mut-helper_anyone.exe"
[ -e "$G/pwned" ] && pass "MUTANT HELPER_ANYONE runs another process's request (gate catches it)" || fail "HELPER_ANYONE not detected: $(cat "$T/probe.out")"
# --- W2. a card cut by the window's bottom edge: its button is still clickable ---------------
# (regression walk 2026-10-05: Archive Manager's Open, the last card of a
# search, did nothing until scrolled into full view)
cutcard() { # exe -- "Y BOTTOM": the cut button's hit and the client area's bottom, or "none"
    store_window "$1" || { echo none; wine taskkill /f /im sg-store64.exe >/dev/null 2>&1; return; }
    hl=$(grep -E '^hit (install|open) ' "$D" | head -1)
    v=$(echo "$hl" | awk '{ print $2 " " $3 }'); y=$(echo "$hl" | awk '{ print $NF }')
    if [ -n "$y" ]; then
        b=$(wine "$T/store-height.exe" $((y + 4)) 2>/dev/null | tr -d '\r' | awk '{ print $2 }')
        sleep 1; xdotool key ctrl+f; sleep 1.5
        y2=$(grep "^hit $v " "$D" | tail -1 | awk '{ print $NF }')
        echo "${y2:-none} ${b:-?} $v"
    else echo none; fi
    wine taskkill /f /im sg-store64.exe >/dev/null 2>&1; sleep 0.5
}
set -- $(cutcard "$EXE")
[ "$1" != none ] && [ -n "${2:-}" ] && [ "$1" -lt "$2" ] 2>/dev/null \
    && pass "a card cut by the window's bottom edge keeps its button clickable ($3 $4 at y $1, the window ends at $2)" \
    || fail "the cut card's button: hit '$*'"
set -- $(cutcard "$T/mut-wholehit.exe")
[ "$1" = none ] && pass "MUTANT WHOLEHIT: the cut card's button is dead (gate catches it)" || fail "WHOLEHIT not detected: $*"

# the window: check boxes, Install selected (2)
ununinstall FakeApp; ununinstall PickApp; sleep 0.5
if store_window "$EXE"; then
    click "$D" "check 01"; sleep 0.8; click "$D" "check 08"; sleep 0.8
    grep -q '^checked 01' "$D" && grep -q '^checked 08' "$D" && grep -q '^hit installsel ' "$D" \
        && pass "two apps checked: Install selected appears" || fail "checks: $(grep -E '^(checked|hit installsel)' "$D")"
    click "$D" "installsel -"; sleep 1.5
    grep -q '^batch 1' "$D" && pass "Install selected installs them as one batch" || fail "no batch: $(grep '^batch' "$D")"
    i=0; while ! { grep -q '^app 01 .*state=\(installed\|done\)' "$D" && grep -q '^app 08 .*state=\(installed\|done\)' "$D" && grep -q '^batch 0' "$D"; } && [ $i -lt 120 ]; do sleep 0.5; i=$((i + 1)); done
    grep -q '^app 01 .*state=\(installed\|done\)' "$D" && grep -q '^app 08 .*state=\(installed\|done\)' "$D" && grep -q '^batch 0' "$D" \
        && pass "both installed, and the batch is over" || fail "after Install selected: $(appline 01) / $(appline 08) / $(grep '^batch' "$D")"
else fail "the window (check boxes) did not open"; fi
wine taskkill /f /im sg-store64.exe >/dev/null 2>&1; sleep 0.5
ununinstall FakeApp; ununinstall PickApp

[ $RC = 0 ] && echo "store-check: all passed" || echo "store-check: FAILED"
exit $RC
