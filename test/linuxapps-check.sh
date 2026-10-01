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
# the user's own copy of an app wins over the system's
{ echo '[Desktop Entry]'; echo 'Type=Application'; echo 'Name=Plain Tool (mine)'; echo 'Exec=true'; } > "$XDG_DATA_HOME/applications/gate-plain.desktop"

"$WINE" "$EXE" --sync; "${WINESERVER:-wineserver}" -w
PROGS="$T/pfx/drive_c/users/$(id -un)/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Linux apps"
list=$(ls "$PROGS" 2>/dev/null | sort | tr '\n' '|')
[ "$list" = "Gate Game.lnk|Plain Tool (mine).lnk|" ] && pass "shown apps get shortcuts; NoDisplay, OnlyShowIn, a missing TryExec, links and the hidden list do not; the user's copy wins" \
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
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
