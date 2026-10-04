#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Linux apps in Start (src/linuxapps): sg-linuxapp64.exe --sync turns the
# .desktop files of the XDG data folders into Programs\Linux apps\*.lnk with
# the apps' own icons; what a Linux desktop would not show is left out; an
# app that goes loses its shortcut; --run starts the app (gio launch).
# Scratch HOME and prefix; the data folders are the gate's (XDG_DATA_DIRS).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
EXE="$HERE/build/sg-linuxapp64.exe"
WINE=${WINE:-wine}
command -v "$WINE" >/dev/null 2>&1 || { echo "SKIP: no wine"; exit 77; }
[ -f "$EXE" ] || { echo "SKIP: build sg-linuxapp64.exe first (make build)"; exit 77; }
RC=0; T=$(mktemp -d /var/tmp/sg-linuxapps.XXXXXX)
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
# no display: nothing of the gate may show on the machine it runs on
unset DISPLAY WAYLAND_DISPLAY
export HOME="$T/home" WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="winemenubuilder.exe=d;mscoree,mshtml="
export XDG_DATA_HOME="$T/home/.local/share" XDG_DATA_DIRS="$T/share" SG_LINUXAPPS_HIDDEN="$T/hidden"
# ~/.config as a session has it: no XDG_CONFIG_HOME, and Wine shows its
# programs no HOME (sg-linuxapp finds it through WINEHOMEDIR)
unset XDG_CONFIG_HOME
trap '"${WINESERVER:-wineserver}" -k 2>/dev/null; rm -rf "$T"' EXIT INT TERM
mkdir -p "$HOME" "$T/share/applications" "$T/share/icons/hicolor/48x48/apps" "$T/share/icons/hicolor/256x256/apps" \
    "$XDG_DATA_HOME/applications"
"$WINE" wineboot -i >/dev/null 2>&1

png() { # FILE SIZE -- a PNG of SIZE x SIZE
    python3 - "$1" "$2" <<'EOF'
import struct, sys, zlib
f, n = sys.argv[1], int(sys.argv[2])
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
raw = b"".join(b"\0" + b"\x20\x80\xe0\xff" * n for _ in range(n))
open(f, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
EOF
}
png "$T/share/icons/hicolor/48x48/apps/gate-game.png" 48
png "$T/share/icons/hicolor/256x256/apps/gate-game.png" 256
app() { # ID then the entry's lines
    id=$1; shift
    { echo '[Desktop Entry]'; for l in "$@"; do echo "$l"; done; } > "$T/share/applications/$id.desktop"
}
app gate-game 'Type=Application' 'Name=Gate Game' 'Comment=A game for the gate' 'Icon=gate-game' "Exec=sh -c 'echo ran > $T/ran'"
app gate-plain 'Type=Application' 'Name=Plain Tool' 'Exec=true'
app gate-nodisplay 'Type=Application' 'Name=Hidden Helper' 'NoDisplay=true' 'Exec=true'
app gate-gnome 'Type=Application' 'Name=Only On GNOME' 'OnlyShowIn=GNOME;' 'Exec=true'
app gate-tryexec 'Type=Application' 'Name=Not Installed' 'TryExec=/nonexistent/prog' 'Exec=/nonexistent/prog'
app gate-link 'Type=Link' 'Name=A Web Link' 'URL=https://example.org'
app debian-gatexterm 'Type=Application' 'Name=GateXTerm' 'Exec=true'
printf '# plumbing\ndebian-gate*\n' > "$T/hidden"
# what came with something else: sg-linuxapp-deps' answer (a stand-in here;
# the helper itself is checked below, on a package database of the gate's)
app gate-dep 'Type=Application' 'Name=Came Along' 'Exec=true'
printf '#!/bin/sh\nprintf "gate-dep\\n" > "$1"\n' > "$T/deps"; chmod 755 "$T/deps"
export SG_LINUXAPP_DEPS="$T/deps"
# the user's own copy of an app wins over the system's
{ echo '[Desktop Entry]'; echo 'Type=Application'; echo 'Name=Plain Tool (mine)'; echo 'Exec=true'; } > "$XDG_DATA_HOME/applications/gate-plain.desktop"

"$WINE" "$EXE" --sync; "${WINESERVER:-wineserver}" -w
PROGS="$T/pfx/drive_c/users/$(id -un)/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Linux apps"
list=$(ls "$PROGS" 2>/dev/null | sort | tr '\n' '|')
[ "$list" = "Gate Game.lnk|Plain Tool (mine).lnk|" ] && pass "shown apps get shortcuts; NoDisplay, OnlyShowIn, a missing TryExec, links, the hidden list and what came with something else do not; the user's copy wins" \
    || fail "shortcuts: $list"
ico="$T/pfx/drive_c/users/$(id -un)/AppData/Local/Stained Glass/Linux app icons/gate-game.ico"
if [ -f "$ico" ] && python3 - "$ico" <<'EOF'
import struct, sys
b = open(sys.argv[1], "rb").read()
z, t, n = struct.unpack("<HHH", b[:6])
sizes = sorted(b[6 + 16 * i] or 256 for i in range(n))
ok = t == 1 and sizes == [48, 256]
for i in range(n):
    size, off = struct.unpack("<II", b[6 + 16 * i + 8: 6 + 16 * i + 16])
    ok = ok and b[off:off + 8] == b"\x89PNG\r\n\x1a\n" and off + size <= len(b)
sys.exit(0 if ok else 1)
EOF
then pass "the app's theme PNGs (48, 256) are its shortcut's icon"; else fail "icon: $(ls -la "$ico" 2>&1)"; fi
strings -el "$PROGS/Gate Game.lnk" 2>/dev/null | grep -q 'gate-game.desktop' && strings -el "$PROGS/Gate Game.lnk" | grep -q 'A game for the gate' \
    && pass "the shortcut runs --run on the app's .desktop, with its Comment" || fail "lnk: $(strings -el "$PROGS/Gate Game.lnk" 2>&1 | head -5)"

rm "$T/share/applications/gate-game.desktop"
"$WINE" "$EXE" --sync; "${WINESERVER:-wineserver}" -w
list=$(ls "$PROGS" 2>/dev/null | sort | tr '\n' '|')
[ "$list" = "Plain Tool (mine).lnk|" ] && pass "an app that goes loses its shortcut" || fail "after removal: $list"

# a Windows program of the same name elsewhere in Programs: the Linux one says so
mkdir -p "$PROGS/../Tools"; cp "$PROGS/Plain Tool (mine).lnk" "$PROGS/../Tools/Plain Tool (mine).lnk"
"$WINE" "$EXE" --sync; "${WINESERVER:-wineserver}" -w
list=$(ls "$PROGS" 2>/dev/null | sort | tr '\n' '|')
[ "$list" = "Plain Tool (mine) (Linux).lnk|" ] && pass "beside a Windows program of the same name it is NAME (Linux)" || fail "same name: $list"

# GNOME's apps ship only an SVG icon: drawn by rsvg-convert; copies named
# after the app's window class (StartupWMClass, Exec's program) for the taskbar
if [ -x /usr/bin/rsvg-convert ]; then
    mkdir -p "$T/share/icons/hicolor/scalable/apps"
    cat > "$T/share/icons/hicolor/scalable/apps/gate-svg.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><rect width="64" height="64" rx="12" fill="#2080e0"/></svg>
EOF
    app gate-svg 'Type=Application' 'Name=Svg App' 'Icon=gate-svg' 'Exec=/usr/bin/gate-svg-prog --new' 'StartupWMClass=GateSvgClass'
    "$WINE" "$EXE" --sync; "${WINESERVER:-wineserver}" -w
    icons="$T/pfx/drive_c/users/$(id -un)/AppData/Local/Stained Glass/Linux app icons"
    if python3 - "$icons/gate-svg.ico" <<'EOF'
import struct, sys
try: b = open(sys.argv[1], "rb").read()
except OSError: sys.exit(1)
z, t, n = struct.unpack("<HHH", b[:6])
sys.exit(0 if t == 1 and sorted(b[6 + 16 * i] or 256 for i in range(n)) == [48, 256] else 1)
EOF
    then pass "an app with only an SVG icon gets an .ico (48, 256)"; else fail "SVG icon: $(ls "$icons" 2>&1 | tr '\n' ' ')"; fi
    cmp -s "$icons/gate-svg.ico" "$icons/GateSvgClass.ico" && cmp -s "$icons/gate-svg.ico" "$icons/gate-svg-prog.ico" \
        && pass "copies named after its window class and program, for the taskbar" || fail "class copies: $(ls "$icons" | tr '\n' ' ')"
    rm "$T/share/applications/gate-svg.desktop"
else
    echo "SKIP  SVG icons (no /usr/bin/rsvg-convert here)"
fi

if [ -x /usr/bin/gio ]; then
    app gate-game 'Type=Application' 'Name=Gate Game' "Exec=sh -c 'echo ran > $T/ran'"
    timeout 60 "$WINE" "$EXE" --run "Z:$(echo "$T" | tr / '\\')\\share\\applications\\gate-game.desktop"
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$T/ran" ] && break; sleep 0.5; done
    [ "$(cat "$T/ran" 2>/dev/null)" = ran ] && pass "--run starts the app (gio launch)" || fail "--run: nothing ran"
else
    echo "SKIP  --run (no /usr/bin/gio here)"
fi
# sg-linuxapp-deps on a package database of the gate's: an app and the
# plumbing it depends on, a wrapper the person installed and the app it wraps,
# and a lone dependency.
D="$T/dpkg"; A="$T/deps-apps"
mkdir -p "$D/info" "$D/updates" "$A"
for id in kapp kplumb wrapped alone; do app2="$A/$id.desktop"; printf '[Desktop Entry]\nType=Application\nName=%s\nExec=true\n' "$id" > "$app2"; done
pkg() { # NAME DEPENDS [DESKTOP]
    printf 'Package: %s\nStatus: install ok installed\nPriority: optional\nSection: misc\nMaintainer: gate\nArchitecture: all\nVersion: 1\n' "$1" >> "$D/status"
    [ -n "$2" ] && printf 'Depends: %s\n' "$2" >> "$D/status"
    printf 'Description: gate\n\n' >> "$D/status"
    if [ -n "${3:-}" ]; then printf '/.\n%s\n' "$A/$3.desktop" > "$D/info/$1.list"; else printf '/.\n' > "$D/info/$1.list"; fi
}
: > "$D/status"
pkg kapp 'kplumb (>= 1), libc6 | libc7' kapp
pkg kplumb '' kplumb
pkg wrapper 'wrapped (>= 1)'
pkg wrapped '' wrapped
pkg alone '' alone
printf 'Package: kplumb\nArchitecture: all\nAuto-Installed: 1\n\nPackage: wrapped\nArchitecture: all\nAuto-Installed: 1\n\nPackage: alone\nArchitecture: all\nAuto-Installed: 1\n' > "$T/extended_states"
if command -v dpkg-query >/dev/null; then
    SG_DPKG_ADMINDIR="$D" SG_LINUXAPP_APPS="$A" SG_APT_EXTENDED_STATES="$T/extended_states" \
        sh "$HERE/src/linuxapps/sg-linuxapp-deps" "$T/deps.out"
    got=$(tr '\n' ' ' < "$T/deps.out" 2>/dev/null)
    [ "$got" = "alone kplumb " ] && pass "sg-linuxapp-deps: a dependency's entries are left out; an app the person's wrapper package is for stays" \
        || fail "sg-linuxapp-deps: [$got]"
else
    echo "SKIP  sg-linuxapp-deps (no dpkg-query here)"
fi
# what the apps open: a ProgID per app for Settings > Default apps and Windows
# programs -- offered for its types' extensions, a browser among the clients,
# gone with the app (Firefox ESR could not be the default browser, David
# 2026-10-02)
rq() { "$WINE" reg query "$1" ${2:+/v "$2"} 2>/dev/null | tr -d '\r'; }
app gate-browser 'Type=Application' 'Name=Gate Browser' 'MimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;' \
    "Exec=sh -c 'echo \"\$1\" > $T/opened' sh %u"
app gate-viewer 'Type=Application' 'Name=Gate Viewer' 'MimeType=image/png;' 'Exec=true'
"$WINE" "$EXE" --sync; "${WINESERVER:-wineserver}" -w
cmd=$(rq 'HKCU\Software\Classes\SG.LinuxApp.gate-browser\shell\open\command')
printf '%s\n' "$cmd" | grep -q -- '--open .*gate-browser.desktop" "%1"' && rq 'HKCU\Software\Classes\.html\OpenWithProgids' | grep -q 'SG.LinuxApp.gate-browser' \
    && rq 'HKCU\Software\Clients\StartMenuInternet\SG Linux gate-browser\Capabilities\URLAssociations' http | grep -q 'SG.LinuxApp.gate-browser' \
    && rq 'HKCU\Software\Classes\.png\OpenWithProgids' | grep -q 'SG.LinuxApp.gate-viewer' \
    && pass "apps that open files or links get ProgIDs: offered for their types' extensions, a browser among the clients" \
    || fail "ProgIDs: $(printf '%s' "$cmd" | tr '\n' ' ') / $(rq 'HKCU\Software\Classes\.html\OpenWithProgids' | tr '\n' ' ')"
rq 'HKCU\Software\Classes\SG.LinuxApp.gate-plain' | grep -q '^HKEY_' && fail "an app that opens nothing got a ProgID" || pass "an app that opens nothing gets none"
if [ -x /usr/bin/gio ]; then
    rm -f "$T/opened"
    timeout 60 "$WINE" "$EXE" --open "Z:$(echo "$T" | tr / '\\')\\share\\applications\\gate-browser.desktop" 'https://example.org/gate'
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$T/opened" ] && break; sleep 0.5; done
    [ "$(cat "$T/opened" 2>/dev/null)" = "https://example.org/gate" ] && pass "--open hands the app the link (gio launch)" || fail "--open: '$(cat "$T/opened" 2>/dev/null)'"
fi
# the default browser (David 2026-10-03: Linux Firefox chosen at
# installation was not the default; Firefox's "make default" did not reach
# Default apps). One browser installed and none chosen: it is the default,
# both sides. Chosen on the Linux side later (xdg-settings, mimeapps.list):
# the Windows side follows, also while --watch runs. A choice made before is
# not undone the first time.
uc() { rq 'HKCU\Software\Microsoft\Windows\Shell\Associations\UrlAssociations\http\UserChoice' ProgId | awk '$1 == "ProgId" { print $3 }'; }
lin() { sed -n 's/^x-scheme-handler\/http=\([^;]*\).*/\1/p' "$HOME/.config/mimeapps.list" 2>/dev/null | head -1; }
if [ -x /usr/bin/xdg-mime ]; then
    [ "$(uc)" = SG.LinuxApp.gate-browser ] && [ "$(lin)" = gate-browser.desktop ] \
        && pass "the one browser installed is the default, Windows and Linux (UserChoice, mimeapps.list)" || fail "one browser: UserChoice '$(uc)', Linux '$(lin)' [$(tr '\n' ' ' < "$HOME/.config/mimeapps.list" 2>&1)]"
    rq 'HKCU\Software\Classes\https\shell\open\command' | grep -q -- '--open .*gate-browser.desktop' && pass "https opens with it" || fail "https not given to it"
    app gate-browser2 'Type=Application' 'Name=Gate Browser Two' 'MimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;' 'Exec=true'
    "$WINE" "$EXE" --sync; "${WINESERVER:-wineserver}" -w
    [ "$(uc)" = SG.LinuxApp.gate-browser ] && pass "a second browser installed does not take over" || fail "a second browser took over: '$(uc)'"
    XDG_DATA_DIRS="$T/share" xdg-mime default gate-browser2.desktop x-scheme-handler/http x-scheme-handler/https text/html
    "$WINE" "$EXE" --sync; "${WINESERVER:-wineserver}" -w
    [ "$(uc)" = SG.LinuxApp.gate-browser2 ] && pass "made default on the Linux side (xdg-mime), Default apps follows" || fail "Linux choice not followed: '$(uc)'"
    "$WINE" "$EXE" --watch & WP=$!
    sleep 4
    XDG_DATA_DIRS="$T/share" xdg-mime default gate-browser.desktop x-scheme-handler/http x-scheme-handler/https text/html
    i=0; while [ "$(uc)" != SG.LinuxApp.gate-browser ] && [ $i -lt 30 ]; do sleep 0.5; i=$((i + 1)); done
    [ "$(uc)" = SG.LinuxApp.gate-browser ] && pass "while --watch runs, a Linux-side choice reaches Default apps within seconds" || fail "--watch did not follow: '$(uc)'"
    kill "$WP" 2>/dev/null; "${WINESERVER:-wineserver}" -k 2>/dev/null; sleep 1
    "$WINE" reg delete 'HKCU\Software\Stained Glass\Default browser' /f >/dev/null 2>&1
    XDG_DATA_DIRS="$T/share" xdg-mime default gate-browser2.desktop x-scheme-handler/http
    "$WINE" "$EXE" --sync; "${WINESERVER:-wineserver}" -w
    [ "$(uc)" = SG.LinuxApp.gate-browser ] && pass "the first time, a choice already made in Settings stays" || fail "first sync undid the choice: '$(uc)'"
    rm -f "$T/share/applications/gate-browser2.desktop"
else
    echo "SKIP  the default browser (no xdg-mime)"
fi
# the app by its program's name (David 2026-10-04: Windows programs and the
# command line could not open Linux Firefox): the user's App Paths get
# NAME.exe -> --launch FILE.desktop, with -esr dropped too; a name a Windows
# program has is left alone; --launch hands every file (as a Unix path) and
# URL to the app; gone with the app
mkdir -p "$T/bin"
printf '#!/bin/sh\nfor a; do echo "$a"; done > %s/named\n' "$T" > "$T/bin/gatenamer-esr"; chmod +x "$T/bin/gatenamer-esr"
app gate-namer 'Type=Application' 'Name=Gate Namer' "Exec=$T/bin/gatenamer-esr %U"
app gate-notepad 'Type=Application' 'Name=Gate Notepad' "Exec=$T/bin/notepad %F"
"$WINE" "$EXE" --sync; "${WINESERVER:-wineserver}" -w
AP='HKCU\Software\Microsoft\Windows\CurrentVersion\App Paths'
args=$(rq "$AP\\gatenamer.exe" SgArguments | sed -n 's/.*REG_SZ *//p')
case "$args" in *'--launch "Z:'*'gate-namer.desktop"') pass "a Linux app answers to its program's name: gatenamer.exe -> --launch its .desktop";;
    *) fail "App Paths gatenamer.exe: '$args'";; esac
rq "$AP\\gatenamer-esr.exe" | grep -q SgArguments && rq "$AP\\gate-namer.exe" | grep -q SgArguments \
    && pass "and to gatenamer-esr and gate-namer (its program, its .desktop name)" || fail "the other names: $(rq "$AP" | tr '\n' ' ')"
rq "$AP\\notepad.exe" | grep -q SgArguments && fail "a Linux app took notepad.exe from Windows' own Notepad" || pass "a name a Windows program has (notepad.exe) is left to it"
if [ -x /usr/bin/gio ]; then
    rm -f "$T/named"; : > "$WINEPREFIX/drive_c/gate doc.txt"
    eval "set -- $args"
    timeout 60 "$WINE" "$EXE" "$@" 'C:\gate doc.txt' 'https://example.org/?a=1&b=2'
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$T/named" ] && break; sleep 0.5; done
    [ "$(realpath "$(sed -n 1p "$T/named" 2>/dev/null)" 2>/dev/null)" = "$(realpath "$WINEPREFIX/drive_c/gate doc.txt")" ] && [ "$(sed -n 2p "$T/named" 2>/dev/null)" = 'https://example.org/?a=1&b=2' ] \
        && pass "--launch gives the app the file (its Unix path) and the URL" || fail "--launch gave: $(tr '\n' '|' < "$T/named" 2>/dev/null)"
fi
rm "$T/share/applications/gate-namer.desktop" "$T/share/applications/gate-notepad.desktop"
"$WINE" "$EXE" --sync; "${WINESERVER:-wineserver}" -w
rq "$AP\\gatenamer.exe" | grep -q SgArguments && fail "an app gone kept its name" || pass "an app that goes loses its names"
rm "$T/share/applications/gate-viewer.desktop"
"$WINE" "$EXE" --sync; "${WINESERVER:-wineserver}" -w
! rq 'HKCU\Software\Classes\SG.LinuxApp.gate-viewer' | grep -q '^HKEY_' && ! rq 'HKCU\Software\Classes\.png\OpenWithProgids' | grep -q 'gate-viewer' \
    && rq 'HKCU\Software\Classes\SG.LinuxApp.gate-browser' | grep -q '^HKEY_' \
    && pass "an app that goes loses its ProgID and its offers; the others stay" || fail "after removal: $(rq 'HKCU\Software\Classes\.png\OpenWithProgids' | tr '\n' ' ')"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
