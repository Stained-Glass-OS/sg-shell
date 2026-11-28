#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Start's Uninstall on a Linux app uninstalls it (src/sg-start.c, src/linuxapps), as Windows'
# Start uninstalls a Store app: before, it opened Programs and Features, which lists no Linux app.
#   - sg-linuxapp64.exe --uninstall FILE.desktop: the package that installed the .desktop file
#     (dpkg's file lists) is removed through SG Store's elevated apt remove
#     (sg-store64.exe --elevated-apt-remove PKG NAME: the administrator's consent, sg-admind
#     refusing what the system needs); a Flatpak by flatpak uninstall (--user for the person's
#     own); an entry no package owns is not touched;
#   - Start's menu for a Linux app: no Run as administrator or Run with debugging (they do not
#     apply), and Uninstall runs the app's launcher with --uninstall and its .desktop file.
# Stand-ins: the store (SG_LINUXAPP_STORE, run directly: SG_STORE_DIRECT=1), dpkg's file lists
# (SG_DPKG_INFO), flatpak (on PATH), the launcher Start's shortcut names.
#   - Settings > Apps & features lists the Linux apps, as Windows' lists its Store apps.
#   mutants: -DSG_MUTANT_START_LINUX_AS_WINDOWS (src/sg-start.c), -DSG_MUTANT_NO_LINUX_APPS_LISTED
#   (src/control/programs.c, sg-settings)
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${SG_WINE:-$WINE_DIR/bin/wine}"
export WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
EXE="${SG_LINUXAPP_EXE:-$HERE/build/sg-linuxapp64.exe}"
START="${SG_START_EXE:-$HERE/build/sg-start64.exe}"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; T=$(mktemp -d /var/tmp/sg-linuxappun.XXXXXX); chmod 755 "$T"; XP=""
W=1280; H=800; BAR=40
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in Xvfb xdotool "$MINGW"; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] && [ -f "$START" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg, sg-start or sg-linuxapp not built"; exit 77; }
unset DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
DPY=$((600 + $$ % 200))
cleanup() { "$WINESERVER" -k 2>/dev/null; command -v sg_kill_prefix >/dev/null && sg_kill_prefix "$WINEPREFIX"; [ -n "$XP" ] && kill "$XP" 2>/dev/null; rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"; }
trap cleanup EXIT INT TERM
export WINEPREFIX="$T/pfx" WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all SG_START_DUMP="$T/dump"
export XDG_DATA_DIRS="$T/share" XDG_DATA_HOME="$T/home/.local/share" SG_LINUXAPP_DEPS=/bin/true SG_LINUXAPPS_HIDDEN="$T/hidden"
: > "$T/hidden"
mkdir -p "$T/share/applications" "$T/bin" "$T/dpkg" "$XDG_DATA_HOME/applications" "$T/home/.local/share/flatpak/exports/share/applications"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/gate-editor"; chmod 755 "$T/bin/gate-editor"
printf '#!/bin/sh\necho "flatpak $*" >> "%s/flatpak.log"\n' "$T" > "$T/bin/flatpak"; chmod 755 "$T/bin/flatpak"
export PATH="$T/bin:$PATH"
printf '[Desktop Entry]\nType=Application\nName=Gate Editor\nExec=gate-editor %%F\n' > "$T/share/applications/org.gate.Editor.desktop"
printf '[Desktop Entry]\nType=Application\nName=Loose Tool\nExec=gate-editor\n' > "$T/share/applications/loose.desktop"
printf '[Desktop Entry]\nType=Application\nName=Gate Flat\nExec=gate-editor\n' > "$T/home/.local/share/flatpak/exports/share/applications/org.gate.Flat.desktop"
printf '/.\n/usr\n%s/share/applications/org.gate.Editor.desktop\n/usr/bin/gate-editor\n' "$T" > "$T/dpkg/gate-editor:amd64.list"
printf '/.\n/usr/share/doc/other\n' > "$T/dpkg/other.list"
"$MINGW" -O2 -municode -o "$T/rec.exe" "$HERE/test/linuxapp-uninstall-rec.c" || { fail "stand-in did not build"; exit 1; }
"$MINGW" -O2 -municode -mwindows -o "$T/poke.exe" "$HERE/test/sg-start-poke.c" 2>/dev/null
"$MINGW" -O2 -municode -o "$T/mklnk.exe" "$HERE/test/sg-start-mklnk.c" -lole32 -luuid -lshell32
"$WINE" wineboot --init >/dev/null 2>&1; "$WINESERVER" -w
export SG_REC="$T/rec.log" SG_DPKG_INFO="$T/dpkg" SG_STORE_DIRECT=1 SG_LINUXAPP_STORE="$(echo "Z:$T/rec.exe" | tr / '\\')"
D() { echo "Z:$1" | tr / '\\'; }
"$WINE" "$EXE" --uninstall "$(D "$T/share/applications/org.gate.Editor.desktop")" /quiet; "$WINESERVER" -w
grep -q -- '--elevated-apt-remove "gate-editor:amd64" "Gate Editor"' "$T/rec.log" 2>/dev/null \
    && pass "a packaged app: its package (dpkg's lists) removed through the store's elevated apt remove" || fail "packaged: $(cat "$T/rec.log" 2>/dev/null)"
: > "$T/rec.log"
"$WINE" "$EXE" --uninstall "$(D "$T/home/.local/share/flatpak/exports/share/applications/org.gate.Flat.desktop")" /quiet; "$WINESERVER" -w
grep -qx 'flatpak uninstall --user -y --noninteractive org.gate.Flat' "$T/flatpak.log" 2>/dev/null \
    && pass "a Flatpak of the person's: flatpak uninstall --user" || fail "flatpak: $(cat "$T/flatpak.log" 2>/dev/null)"
"$WINE" "$EXE" --uninstall "$(D "$T/share/applications/loose.desktop")" /quiet; rc=$?; "$WINESERVER" -w
[ ! -s "$T/rec.log" ] && [ "$rc" != 0 ] && pass "an entry no package owns is not touched" || fail "loose entry: rc $rc, $(cat "$T/rec.log")"

# Settings > Apps & features lists the Linux apps (as Windows' its Store apps); Programs and
# Features does not
SETTINGS="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
if [ -f "$SETTINGS" ]; then
    K='HKCU\Software\Classes\AppUserModelId\SG.LinuxApp.org.gate.Editor'
    "$WINE" reg add "$K" /v DisplayName /d 'Gate Editor' /f >/dev/null 2>&1
    "$WINE" reg add "$K" /v RelaunchCommand /d "\"$(D "$T/launcher-s/sg-linuxapp64.exe")\" --run \"$(D "$T/share/applications/org.gate.Editor.desktop")\"" /f >/dev/null 2>&1
    "$WINESERVER" -w
    Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/sdisplay" >/dev/null 2>&1 & SXP=$!
    i=0; while [ ! -s "$T/sdisplay" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    rm -f "$T/settings.dump"
    DISPLAY=":$(cat "$T/sdisplay")" SG_SETTINGS_DUMP="$(D "$T/settings.dump")" "$WINE" "$(D "$SETTINGS")" ms-settings:appsfeatures >/dev/null 2>&1 &
    i=0; while ! grep -q '^page ' "$T/settings.dump" 2>/dev/null && [ $i -lt 120 ]; do sleep 0.25; i=$((i + 1)); done
    sleep 1; tr -d '\r' < "$T/settings.dump" 2>/dev/null | sed 's/^\xEF\xBB\xBF//' > "$T/sd"
    "$WINESERVER" -k 2>/dev/null; "$WINESERVER" -w 2>/dev/null; kill "$SXP" 2>/dev/null
    grep -qx 'text Linux app' "$T/sd" && pass "Settings > Apps & features lists the Linux app" || fail "Apps & features: $(grep -c '^text' "$T/sd") texts, no Linux app"
else
    echo "SKIP  Settings > Apps & features (no sg-settings64.exe)"
fi

# Start: the Linux app's menu and its Uninstall
U=$(id -un)
LINUXPROG='C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Linux apps'
mkdir -p "$T/launcher"; cp "$T/rec.exe" "$T/launcher/sg-linuxapp64.exe"
cat > "$T/mklnk2.c" <<'EOF'
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
int wmain(int argc, WCHAR **argv)
{
    IShellLinkW *l; IPersistFile *f;
    if (argc < 4) return 2;
    CoInitialize(NULL);
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&l))) return 1;
    IShellLinkW_SetPath(l, argv[2]); IShellLinkW_SetArguments(l, argv[3]);
    IShellLinkW_QueryInterface(l, &IID_IPersistFile, (void **)&f);
    return FAILED(IPersistFile_Save(f, argv[1], TRUE));
}
EOF
"$MINGW" -O2 -municode -o "$T/mklnk2.exe" "$T/mklnk2.c" -lole32 -luuid || { fail "link maker did not build"; exit 1; }
mkdir -p "$WINEPREFIX/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs/Linux apps"
DESK=$(D "$T/share/applications/org.gate.Editor.desktop")
"$WINE" "$T/mklnk2.exe" "$LINUXPROG\\Gate Editor.lnk" "$(D "$T/launcher/sg-linuxapp64.exe")" "--run \"$DESK\"" >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d ${W}x${H} /f >/dev/null 2>&1
"$WINESERVER" -w
rm -f "/tmp/.X${DPY}-lock"
Xvfb ":$DPY" -screen 0 ${W}x${H}x24 -nolisten tcp >/dev/null 2>&1 & XP=$!; sleep 2
export DISPLAY=":$DPY"
"$WINE" explorer /desktop=shell,${W}x${H} > "$T/explorer.log" 2>&1 &
i=0; while [ $i -lt 60 ] && ! xdotool search --name 'shell - Wine Desktop' >/dev/null 2>&1; do sleep 0.5; i=$((i+1)); done
sleep 2
# Start's own Linux apps watcher (sg-linuxapp --watch, beside it) finds no apps here: the gate's
# shortcut is the all-users one it does not keep
mkdir -p "$T/empty"; export XDG_DATA_DIRS="$T/empty" XDG_DATA_HOME="$T/empty"
"$WINE" "$START" >/dev/null 2>&1 &
sleep 5
d() { cat "$T/dump" 2>/dev/null; }
val() { d | sed -n "s/^$1=//p"; }
panel_y=$((H - BAR - 640))
: > "$T/rec.log"
"$WINE" "$T/poke.exe" >/dev/null 2>&1; sleep 2
xdotool type --delay 80 'gate editor'; sleep 1.5
xdotool mousemove 110 $((panel_y + 112)) click 3; sleep 1.5
items=$(val menu_items)
[ "$(val menu)" = context ] && case "$items" in *Uninstall*) ! echo "$items" | grep -q 'Run as administrator\|Run with debugging' ;; *) false ;; esac \
    && pass "a Linux app's menu: no Run as administrator or Run with debugging ($items)" || fail "Linux app's menu: $(val menu) $items"
xdotool key u; sleep 3
grep -qF -- "--uninstall \"$DESK\"" "$T/rec.log" && pass "its Uninstall runs the app's launcher with --uninstall and its .desktop file" \
    || fail "Uninstall ran: '$(cat "$T/rec.log")' (launched $(val launched))"
echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
