#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# AppImages a person downloaded, installed for them (src/linuxapps/sg-appimage,
# sg-store64.exe --appimage, sg-linuxapp64.exe): David 2026-10-07.
#
#   A. sg-appimage on its own (no Wine), on a stand-in AppImage the gate
#      builds -- an ELF "runtime" (test/appimage-runtime.c) with "AI\2" at
#      byte 8 and a SquashFS appended holding a desktop entry, an icon and
#      an AppRun that writes a marker:
#      - info names the app (name, id, version) without running it;
#      - install copies it into ~/Applications (executable; the download
#        kept), writes ~/.local/share/applications/appimage-<id>.desktop
#        whose Exec is ours ("sg-appimage run FILE %U": of the image's own
#        Exec line only %U is kept), TryExec, X-SG-AppImage, and the icon
#        into ~/.local/share/icons/hicolor/48x48/apps/; nothing ran;
#      - refused, saying why: a type 1 AppImage, a file that is not one, an
#        AppImage for another processor, one with no program image;
#      - run starts it, with APPIMAGE_EXTRACT_AND_RUN=1 when FUSE cannot be
#        used; an Applications folder whose path has a space and a $ still
#        starts through gio launch (the Exec quoting);
#      - a newer version of the same app (same desktop id, another file
#        name) replaces the old one: one entry, the old file gone;
#      - --move takes the download away; uninstall removes the file, the
#        entry and the icons.
#   B. the Windows side (Xvfb, wine-sg):
#      - sg-linuxapp64.exe --sync: Start's "Linux apps\Gate Tool.lnk" with
#        the app's icon, an Uninstall entry (HKCU ... Uninstall\SG.AppImage.
#        gatetool: name, version, --uninstall-appimage) for Settings > Apps;
#      - --run of its entry starts the AppImage (gio launch, our Exec);
#      - the "Install an AppImage" window (sg-store64.exe --appimage):
#        name and file shown, Install installs it and Start lists it at once
#        (no separate sync); a newer version with "move" ticked replaces it
#        and takes the download away; double-clicking a .AppImage (HKCR
#        from defaults/88-sg-linuxapps.reg) opens the window, and so does
#        a Unix path (Linux Firefox's download, sg-session);
#      - the Uninstall entry's QuietUninstallString removes it all: file,
#        entry, Start shortcut, Uninstall key.
#
# Mutants (test/appimage-mutants.sh runs them; each must FAIL here):
#   SG_MUTANT_KEEP_EXEC=1           the image's own Exec line is trusted
#   SG_MUTANT_NO_REPLACE=1          a newer version is a second entry
#   SG_MUTANT_KEEP_FILE=1           uninstall leaves the AppImage behind
#   SG_MUTANT_NO_EXTRACT_FALLBACK=1 no APPIMAGE_EXTRACT_AND_RUN without FUSE
#   SG_MUTANT_ANY_MACHINE=1         another processor's AppImage accepted
#   C: SG_MUTANT_NO_APPIMAGE_UNINSTALL (sg-linuxapp.c) no Uninstall entry;
#      SG_MUTANT_AI_NO_SYNC (sysinstall.c) Start not brought up to date
#
# Needs gcc, mksquashfs/unsquashfs, python3, gio; B also wine-sg
# (SG_WINE_DIR, default /opt/wine-sg), Xvfb and xdotool (B is skipped
# without them). SG_LINUXAPP_EXE / SG_STORE_EXE test other builds.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
HELPER="$HERE/src/linuxapps/sg-appimage"
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
LEXE="${SG_LINUXAPP_EXE:-$HERE/build/sg-linuxapp64.exe}"
SEXE="${SG_STORE_EXE:-$HERE/build/sg-store64.exe}"
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in gcc mksquashfs unsquashfs python3; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done

T=$(mktemp -d /var/tmp/sg-appimage-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    [ -x "$WINE_DIR/bin/wineserver" ] && WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T" /tmp/sg-gate-appimage.* 2>/dev/null
}
trap cleanup EXIT INT TERM
export SG_APPIMAGE_SELF="$HELPER" SG_APPIMAGE_DIR="$HOME/Applications"
DATA="$XDG_DATA_HOME"
ENTRY="$DATA/applications/appimage-gatetool.desktop"
MARK="$T/mark"

# --- the stand-in AppImages ----------------------------------------------------------------------
gcc -O2 -s -o "$T/runtime" "$HERE/test/appimage-runtime.c" || { echo "FAIL  the stand-in runtime does not build"; exit 1; }
png() { python3 - "$1" "$2" <<'EOF'
import struct, sys, zlib
f, n = sys.argv[1], int(sys.argv[2])
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
raw = b"".join(b"\0" + b"\x20\x80\xe0\xff" * n for _ in range(n))
open(f, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
EOF
}
# mkimage OUT VERSION [MAGIC]: Gate Tool, version VERSION; MAGIC the bytes at 8 (default AI\2)
mkimage() {
    a="$T/AppDir-$2"; rm -rf "$a"; mkdir -p "$a/usr/share/icons/hicolor/48x48/apps"
    cat > "$a/gatetool.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Gate Tool
Comment=A tool for the gate\\nsecond line
Exec=gatetool --flag \$(touch $T/pwned) %U
Icon=gatetool
Categories=Utility;
X-AppImage-Version=$2
EOF
    printf '#!/bin/sh\necho "%s $SG_GATE_EXTRACT $*" > "%s"\n' "$2" "$MARK" > "$a/AppRun"; chmod 755 "$a/AppRun"
    png "$a/usr/share/icons/hicolor/48x48/apps/gatetool.png" 48
    ln -s usr/share/icons/hicolor/48x48/apps/gatetool.png "$a/.DirIcon"
    mksquashfs "$a" "$T/sq.img" -quiet -noappend -root-owned >/dev/null 2>&1
    cat "$T/runtime" "$T/sq.img" > "$1"
    printf "${3:-AI\\002}" | dd of="$1" bs=1 seek=8 conv=notrunc 2>/dev/null
    chmod 644 "$1"
}
mkdir -p "$HOME/Downloads"
V1="$HOME/Downloads/Gate_Tool-1.0-x86_64.AppImage"; V2="$HOME/Downloads/Gate_Tool-2.0-x86_64.AppImage"
mkimage "$V1" 1.0
mkimage "$V2" 2.0
mkimage "$T/old.AppImage" 0.1 'AI\001'
printf 'just text\n' > "$T/text.AppImage"
cp "$T/runtime" "$T/noimage.AppImage"
cp "$V1" "$T/arm.AppImage"; printf '\267\000' | dd of="$T/arm.AppImage" bs=1 seek=18 conv=notrunc 2>/dev/null   # e_machine aarch64
field() { sed -n "s/^$2 //p" "$1" | head -1; }
key() { sed -n "s/^$2=//p" "$1" 2>/dev/null | head -1; }

# --- A. sg-appimage ---------------------------------------------------------------------------
"$HELPER" info "$V1" --out "$T/info" >/dev/null 2>&1
[ "$(field "$T/info" NAME)" = "Gate Tool" ] && [ "$(field "$T/info" ID)" = gatetool ] && [ "$(field "$T/info" VERSION)" = 1.0 ] \
    && grep -q '^OK' "$T/info" && pass "info: Gate Tool, id gatetool, version 1.0" || fail "info: $(tr '\n' '|' < "$T/info" 2>/dev/null)"
for bad in old text noimage arm; do
    "$HELPER" info "$T/$bad.AppImage" --out "$T/info-$bad" >/dev/null 2>&1
    e=$(field "$T/info-$bad" ERROR)
    case "$bad:$e" in
        old:*"type 1"*|text:*"not an AppImage"*|noimage:*"not an AppImage"*|arm:*"another kind of processor"*) pass "refused ($bad): $e" ;;
        *) fail "$bad: not refused as it should be: $(tr '\n' '|' < "$T/info-$bad" 2>/dev/null)" ;;
    esac
done
"$HELPER" install "$V1" --out "$T/inst1" >/dev/null 2>&1
F1="$HOME/Applications/Gate_Tool-1.0-x86_64.AppImage"
grep -q '^OK' "$T/inst1" && [ -x "$F1" ] && cmp -s "$F1" "$V1" && [ -f "$V1" ] \
    && pass "install: copied into ~/Applications, executable; the download is kept" || fail "install: $(tr '\n' '|' < "$T/inst1" 2>/dev/null) / $(ls -la "$HOME/Applications" 2>&1)"
[ ! -e "$MARK" ] && [ ! -e "$T/pwned" ] && pass "reading and installing it ran nothing of it" || fail "something of the AppImage ran: $(cat "$MARK" 2>/dev/null)"
ex=$(key "$ENTRY" Exec)
[ "$ex" = "$HELPER run $F1 %U" ] && pass "its entry's Exec is ours, with the image's %U only: $ex" || fail "Exec: '$ex'"
grep -q 'touch\|\$(' "$ENTRY" 2>/dev/null && fail "the image's own command reached the entry" || pass "the image's own command line is not in the entry"
[ "$(key "$ENTRY" TryExec)" = "$F1" ] && [ "$(key "$ENTRY" X-SG-AppImage)" = "$F1" ] && [ "$(key "$ENTRY" Name)" = "Gate Tool" ] \
    && [ "$(key "$ENTRY" Icon)" = appimage-gatetool ] && [ "$(key "$ENTRY" Comment)" = "A tool for the gate second line" ] \
    && pass "TryExec, X-SG-AppImage, Name, one-line Comment, Icon=appimage-gatetool" || fail "entry: $(tr '\n' '|' < "$ENTRY" 2>/dev/null)"
ICON="$DATA/icons/hicolor/48x48/apps/appimage-gatetool.png"
cmp -s "$ICON" "$T/AppDir-1.0/usr/share/icons/hicolor/48x48/apps/gatetool.png" && pass "its icon is in ~/.local/share/icons/hicolor/48x48/apps" || fail "icon: $(find "$DATA/icons" -type f 2>&1 | tr '\n' ' ')"
"$HELPER" list | grep -q "^gatetool	Gate Tool	1.0	$F1" && pass "list names it" || fail "list: $("$HELPER" list)"

rm -f "$MARK"
SG_APPIMAGE_FUSE=/nonexistent "$HELPER" run "$F1" one two
[ "$(cat "$MARK" 2>/dev/null)" = "1.0 1 one two" ] && pass "run starts it with its arguments; without FUSE with APPIMAGE_EXTRACT_AND_RUN=1" || fail "run: '$(cat "$MARK" 2>/dev/null)'"
if [ -r /dev/fuse ] && [ -w /dev/fuse ] && command -v fusermount3 >/dev/null && python3 -c 'import ctypes.util,sys; sys.exit(0 if ctypes.util.find_library("fuse") else 1)'; then
    rm -f "$MARK"; "$HELPER" run "$F1"
    [ "$(cat "$MARK" 2>/dev/null)" = "1.0 0 " ] && pass "with FUSE it is left to mount itself" || fail "run with FUSE: '$(cat "$MARK" 2>/dev/null)'"
fi
# an Applications folder with a space and a $ in its path: the Exec quoting, through gio launch
if command -v gio >/dev/null; then
    odd="$T/odd home \$x"; mkdir -p "$odd/data"
    SG_APPIMAGE_DIR="$odd/Applications" XDG_DATA_HOME="$odd/data" "$HELPER" install "$V1" >/dev/null 2>&1
    rm -f "$MARK"
    SG_APPIMAGE_FUSE=/nonexistent gio launch "$odd/data/applications/appimage-gatetool.desktop" "$T/a file.txt" 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -s "$MARK" ] && break; sleep 0.25; done
    [ "$(cat "$MARK" 2>/dev/null)" = "1.0 1 $T/a file.txt" ] && pass "gio launch of its entry starts it, from a folder named with a space and a \$" \
        || fail "gio launch (odd folder): '$(cat "$MARK" 2>/dev/null)' / $(key "$odd/data/applications/appimage-gatetool.desktop" Exec)"
else echo "SKIP  gio launch (no gio)"; fi

# a newer version: replaces it
"$HELPER" install "$V2" --out "$T/inst2" >/dev/null 2>&1
F2="$HOME/Applications/Gate_Tool-2.0-x86_64.AppImage"
n=$(ls "$DATA/applications"/appimage-*.desktop 2>/dev/null | wc -l)
[ "$(field "$T/inst2" REPLACED)" = yes ] && [ "$n" = 1 ] && [ -x "$F2" ] && [ ! -e "$F1" ] && [ "$(key "$ENTRY" X-AppImage-Version)" = 2.0 ] \
    && [ "$(key "$ENTRY" X-SG-AppImage)" = "$F2" ] \
    && pass "a newer version replaces the old one: one entry, version 2.0, the old file gone" \
    || fail "newer version: $n entries, $(ls "$HOME/Applications" | tr '\n' ' ') / $(tr '\n' '|' < "$T/inst2" 2>/dev/null)"
"$HELPER" uninstall gatetool --out "$T/un" >/dev/null 2>&1
[ ! -e "$F2" ] && [ ! -e "$ENTRY" ] && [ ! -e "$ICON" ] && grep -q '^OK' "$T/un" \
    && pass "uninstall removes the AppImage, its entry and its icon" || fail "uninstall: $(ls -R "$HOME/Applications" "$DATA" 2>&1 | tr '\n' ' ')"
cp "$V1" "$T/move.AppImage"
"$HELPER" install --move "$T/move.AppImage" >/dev/null 2>&1
[ ! -e "$T/move.AppImage" ] && [ -x "$HOME/Applications/move.AppImage" ] && pass "--move takes the download away" || fail "--move: $(ls "$T" "$HOME/Applications" | tr '\n' ' ')"
"$HELPER" uninstall "$ENTRY" >/dev/null 2>&1
[ ! -e "$HOME/Applications/move.AppImage" ] && [ ! -e "$ENTRY" ] && pass "uninstall by its entry's path" || fail "uninstall by path"

# --- B. the Windows side ---------------------------------------------------------------------
if ! [ -x "$WINE_DIR/bin/wine" ] || ! [ -f "$LEXE" ] || ! [ -f "$SEXE" ] || ! command -v Xvfb >/dev/null || ! command -v xdotool >/dev/null; then
    echo "SKIP  the Windows side (wine-sg in $WINE_DIR, the build, Xvfb or xdotool missing)"
    [ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
    exit "$RC"
fi
Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
DPY=$(cat "$T/display" 2>/dev/null); [ -n "$DPY" ] || { echo "FAIL  Xvfb did not start"; exit 1; }
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d'
export PATH="$WINE_DIR/bin:$PATH" XDG_DATA_DIRS="$T/share" SG_LINUXAPPS_HIDDEN="$T/hidden" SG_LINUXAPP_DEPS=/nonexistent
export SG_APPIMAGE_HELPER="$HELPER" SG_APPIMAGE_FUSE=/nonexistent
mkdir -p "$T/share/applications"; : > "$T/hidden"
wine wineboot --init >/dev/null 2>&1; wineserver -w
w() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }
rq() { wine reg query "$1" ${2:+/v "$2"} 2>/dev/null | tr -d '\r'; }
export SG_LINUXAPP_EXE="$(w "$LEXE")"
PROGS="$T/pfx/drive_c/users/$(id -un)/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Linux apps"
UK='HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\SG.AppImage.gatetool'

"$HELPER" install "$V1" >/dev/null 2>&1
wine "$LEXE" --sync; wineserver -w
[ -f "$PROGS/Gate Tool.lnk" ] && pass "Start lists it: Linux apps\\Gate Tool.lnk" || fail "Start: $(ls "$PROGS" 2>&1 | tr '\n' ' ')"
ico="$T/pfx/drive_c/users/$(id -un)/AppData/Local/Stained Glass/Linux app icons/appimage-gatetool.ico"
[ -s "$ico" ] && pass "with the AppImage's icon (appimage-gatetool.ico)" || fail "no icon made: $(ls "$(dirname "$ico")" 2>&1 | tr '\n' ' ')"
un=$(rq "$UK" UninstallString | sed -n 's/.*REG_SZ *//p')
rq "$UK" DisplayName | grep -q 'Gate Tool' && rq "$UK" DisplayVersion | grep -q '1\.0' && case "$un" in *'--uninstall-appimage "'*'appimage-gatetool.desktop"') true ;; *) false ;; esac \
    && pass "an Uninstall entry for Settings > Apps: Gate Tool 1.0, --uninstall-appimage" || fail "Uninstall key: $(rq "$UK" | tr '\n' ' ')"
rm -f "$MARK"
timeout 60 wine "$LEXE" --run "$(w "$ENTRY")"
for _ in $(seq 1 40); do [ -s "$MARK" ] && break; sleep 0.25; done
[ "$(cat "$MARK" 2>/dev/null)" = "1.0 1 " ] && pass "Start's shortcut (--run its entry) starts it" || fail "--run: '$(cat "$MARK" 2>/dev/null)'"
"$HELPER" uninstall gatetool >/dev/null 2>&1
wine "$LEXE" --sync; wineserver -w
[ ! -e "$PROGS/Gate Tool.lnk" ] && ! rq "$UK" | grep -q '^HKEY' && pass "gone from the Linux side: its shortcut and Uninstall entry go at the next sync" || fail "after removal: $(ls "$PROGS" 2>&1) / $(rq "$UK" | head -2)"

# the desktop the windows live on
wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768 /f >/dev/null 2>&1
wineserver -w
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
click() { xy=$(grep "^hit $2 " "$1" 2>/dev/null | tail -1 | awk '{ print $(NF-1), $NF }'); [ -n "$xy" ] || return 1
          xdotool mousemove "${xy% *}" "${xy#* }" click 1; }
waitfor() { i=0; while ! grep -q "$2" "$1" 2>/dev/null && [ $i -lt "${3:-80}" ]; do sleep 0.25; i=$((i + 1)); done; grep -q "$2" "$1" 2>/dev/null; }
D="$T/dump"; export SG_STORE_DUMP="$(w "$D")"

rm -f "$D"
wine "$SEXE" --appimage "$(w "$V1")" >/dev/null 2>&1 & WP=$!
if waitfor "$D" '^hit install ' 120; then
    [ "$(field "$D" name)" = "Gate Tool" ] && [ "$(field "$D" version)" = 1.0 ] && [ "$(field "$D" file)" = "Gate_Tool-1.0-x86_64.AppImage" ] \
        && pass "the Install an AppImage window names it: Gate Tool 1.0, from Gate_Tool-1.0-x86_64.AppImage" || fail "window: $(tr '\n' '|' < "$D")"
    [ ! -e "$F1" ] && pass "...and nothing is installed before Install" || fail "installed before Install was clicked"
    command -v import >/dev/null && import -window root "$HERE/build/appimage-window.png" 2>/dev/null
    click "$D" install
    if waitfor "$D" '^result ' 240; then
        [ "$(field "$D" result | cut -d' ' -f1)" = 0 ] && [ -x "$F1" ] && [ -f "$ENTRY" ] && pass "Install installs it: $(field "$D" result | cut -d' ' -f2-)" || fail "Install: $(tr '\n' '|' < "$D")"
        [ -f "$PROGS/Gate Tool.lnk" ] && rq "$UK" | grep -q DisplayName && pass "Start lists it at once (and its Uninstall entry is there)" || fail "Start not brought up to date: $(ls "$PROGS" 2>&1 | tr '\n' ' ')"
        click "$D" close
    else fail "the install did not finish: $(tr '\n' '|' < "$D")"; fi
else fail "the Install an AppImage window did not open: $(cat "$D" 2>/dev/null)"; fi
sleep 0.5; kill "$WP" 2>/dev/null; wait "$WP" 2>/dev/null

rm -f "$D"
wine "$SEXE" --appimage "$(w "$V2")" >/dev/null 2>&1 & WP=$!
if waitfor "$D" '^hit install ' 120; then
    click "$D" move; waitfor "$D" '^move 1' 20
    click "$D" install
    if waitfor "$D" '^result ' 240; then
        n=$(ls "$PROGS" 2>/dev/null | wc -l)
        [ -x "$F2" ] && [ ! -e "$F1" ] && [ ! -e "$V2" ] && [ "$n" = 1 ] && rq "$UK" DisplayVersion | grep -q '2\.0' \
            && pass "a newer version through the window (move ticked) replaces it: one Start entry, version 2.0, the download moved" \
            || fail "newer version: $(ls "$HOME/Applications" "$HOME/Downloads" "$PROGS" 2>&1 | tr '\n' ' ') / $(rq "$UK" DisplayVersion | tr '\n' ' ')"
        click "$D" close
    else fail "the second install did not finish: $(tr '\n' '|' < "$D")"; fi
else fail "the window did not open for 2.0: $(cat "$D" 2>/dev/null)"; fi
sleep 0.5; kill "$WP" 2>/dev/null; wait "$WP" 2>/dev/null

# File Explorer's double-click: HKCR\.AppImage from defaults/88-sg-linuxapps.reg, pointed at this build
sed "s|Z:\\\\\\\\usr\\\\\\\\libexec\\\\\\\\stained-glass\\\\\\\\shell\\\\\\\\sg-store64.exe|$(w "$SEXE" | sed 's/\\/\\\\\\\\/g')|g" \
    "$HERE/defaults/88-sg-linuxapps.reg" > "$T/assoc.reg"
wine regedit /S "$(w "$T/assoc.reg")" >/dev/null 2>&1
cmd=$(rq 'HKLM\Software\Classes\SG.AppImage\shell\open\command' | sed -n 's/.*REG_SZ *//p')
case "$cmd" in *sg-store64.exe*--appimage*'%1'*) pass "HKCR\\.AppImage installs with --appimage \"%1\"" ;; *) fail ".AppImage verb: '$cmd'" ;; esac
mkimage "$T/dbl.AppImage" 3.0
rm -f "$D"
wine start /unix "$T/dbl.AppImage" >/dev/null 2>&1
if waitfor "$D" '^appimage-window 1' 120; then pass "double-clicking an .AppImage opens the Install an AppImage window"; click "$D" cancel
else fail "ShellExecute of an .AppImage did not open the window: $(cat "$D" 2>/dev/null)"; fi
sleep 0.5; wine taskkill /f /im sg-store64.exe >/dev/null 2>&1
# Linux Firefox's download opened (sg-session's sg-open-windows-file --appimage): a Unix path
rm -f "$D"
wine "$SEXE" --appimage "$T/dbl.AppImage" >/dev/null 2>&1 & WP=$!
if waitfor "$D" '^hit install ' 120 && [ "$(field "$D" file)" = dbl.AppImage ] && [ "$(field "$D" version)" = 3.0 ]; then
    pass "given the file's Unix path (from a Linux program) the window reads it too"; click "$D" cancel
else fail "Unix path: $(tr '\n' '|' < "$D" 2>/dev/null)"; fi
sleep 0.5; kill "$WP" 2>/dev/null; wait "$WP" 2>/dev/null

# Settings > Apps' Uninstall (its quiet command, as Settings has asked already)
q=$(rq "$UK" QuietUninstallString | sed -n 's/.*REG_SZ *//p')
eval "set -- $(printf '%s' "$q" | sed 's/\\/\\\\/g')"
timeout 120 wine "$@"
[ ! -e "$F2" ] && [ ! -e "$ENTRY" ] && [ ! -e "$PROGS/Gate Tool.lnk" ] && ! rq "$UK" | grep -q '^HKEY' \
    && pass "its Uninstall entry removes it: the file, the entry, the Start shortcut, the Uninstall key" \
    || fail "uninstall entry: $(ls "$HOME/Applications" "$PROGS" 2>&1 | tr '\n' ' ') / $(rq "$UK" | head -1) [$q]"

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
