#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# A Linux app is an app to the taskbar as a Windows program is (src/linuxapps, with wine-sg
# 9511): sg-linuxapp64.exe --sync gives each app's Start shortcut the AppUserModelID
# SG.LinuxApp.<desktop id>, registers the ID (HKCU\Software\Classes\AppUserModelId\<ID>:
# DisplayName, IconUri, RelaunchCommand, SgShortcut), names the app by its windows' classes
# (HKCU\Software\Stained Glass\Linux apps\Window classes: desktop id, StartupWMClass, Exec's
# program -- not a program several apps start) and makes its .desktop actions its jump list's
# tasks (ICustomDestinationList), each started with --action through GLib. An app that goes
# loses all of it. Before, a Linux window's button was "\sg-linux\<class>": the app pinned
# from Start showed twice, its menu named the class and pinned nothing, no jump list.
# The files a Linux program opened (recently-used.xbel, GTK's recent files) go to its app's recent
# documents (SHAddToRecentDocs with its ID): its jump list's Recent.
#   mutants: -DSG_MUTANT_NO_AUMID, -DSG_MUTANT_NO_WINDOW_CLASSES, -DSG_MUTANT_NO_RECENT (src/linuxapps/sg-linuxapp.c)
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
EXE="${SG_LINUXAPP_EXE:-$HERE/build/sg-linuxapp64.exe}"
WINE=${WINE:-wine}
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
command -v "$WINE" >/dev/null 2>&1 || { echo "SKIP: no wine"; exit 77; }
command -v "$MINGW" >/dev/null 2>&1 || { echo "SKIP: no $MINGW"; exit 77; }
[ -f "$EXE" ] || { echo "SKIP: build sg-linuxapp64.exe first (make build)"; exit 77; }
RC=0; T=$(mktemp -d /var/tmp/sg-linuxappid.XXXXXX)
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
unset DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
export HOME="$T/home" WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="winemenubuilder.exe=d;mscoree,mshtml="
export XDG_DATA_HOME="$T/home/.local/share" XDG_DATA_DIRS="$T/share" SG_LINUXAPPS_HIDDEN="$T/hidden" SG_LINUXAPP_DEPS=/bin/true
unset XDG_CONFIG_HOME
WINESERVER=${WINESERVER:-$(dirname "$(command -v "$WINE")")/wineserver}
trap '"$WINESERVER" -k 2>/dev/null; command -v sg_kill_prefix >/dev/null && sg_kill_prefix "$WINEPREFIX"; rm -rf "$T"' EXIT INT TERM
mkdir -p "$HOME" "$T/share/applications" "$XDG_DATA_HOME/applications" "$T/bin"
# the app's program, on the PATH (GLib starts no entry whose program is missing)
printf '#!/bin/sh\nexit 0\n' > "$T/bin/gate-editor"; chmod 755 "$T/bin/gate-editor"
export PATH="$T/bin:$PATH"
: > "$T/hidden"
"$MINGW" -municode -O2 -o "$T/probe.exe" "$HERE/test/linuxapp-identity-probe.c" -lole32 -luuid || { echo "FAIL  probe did not build"; exit 1; }
"$WINE" wineboot -i >/dev/null 2>&1
app() { id=$1; shift; { echo '[Desktop Entry]'; for l in "$@"; do echo "$l"; done; } > "$T/share/applications/$id.desktop"; }
app org.gate.Editor 'Type=Application' 'Name=Gate Editor' "Exec=gate-editor %F" 'StartupWMClass=GateEditorWin' \
    'Actions=new-window;private;nameless;'
cat >> "$T/share/applications/org.gate.Editor.desktop" <<EOF

[Desktop Action new-window]
Name=New Window
Exec=sh -c "echo new-window > $T/action"

[Desktop Action private]
Name=New Private Window
Exec=sh -c "echo private > $T/action"

[Desktop Action nameless]
Exec=true
EOF
# a launcher's games run the launcher's program: that program names none of them
app gate-game-a 'Type=Application' 'Name=Game A' 'Exec=gate-launcher run://1'
app gate-game-b 'Type=Application' 'Name=Game B' 'Exec=gate-launcher run://2'
"$WINE" "$EXE" --sync; "$WINESERVER" -w

U=$(id -un)
LNK='C:\users\'"$U"'\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Linux apps\Gate Editor.lnk'
ID=SG.LinuxApp.org.gate.Editor
q() { "$WINE" reg query "$1" /v "$2" 2>/dev/null | tr -d '\r' | awk -v n="$2" '$1 == n { $1 = ""; $2 = ""; sub(/^  /, ""); print; exit }'; }
got=$("$WINE" "$T/probe.exe" appid "$LNK" 2>/dev/null | tr -d '\r')
[ "$got" = "$ID" ] && pass "the app's shortcut carries its AppUserModelID ($ID)" || fail "shortcut's ID: '$got'"
K="HKCU\\Software\\Classes\\AppUserModelId\\$ID"
[ "$(q "$K" DisplayName)" = "Gate Editor" ] && pass "the ID is registered with the app's name (what the taskbar and its toasts name it by)" || fail "DisplayName: '$(q "$K" DisplayName)'"
case "$(q "$K" RelaunchCommand)" in *sg-linuxapp64.exe*--run*org.gate.Editor.desktop*) pass "and how another window of it starts (RelaunchCommand: --run its .desktop)" ;;
    *) fail "RelaunchCommand: '$(q "$K" RelaunchCommand)'" ;; esac
[ "$(q "$K" SgShortcut)" = "$LNK" ] && pass "and its Start shortcut (what pinning copies)" || fail "SgShortcut: '$(q "$K" SgShortcut)'"
W='HKCU\Software\Stained Glass\Linux apps\Window classes'
[ "$(q "$W" org.gate.Editor)" = "$ID" ] && [ "$(q "$W" GateEditorWin)" = "$ID" ] && [ "$(q "$W" gate-editor)" = "$ID" ] \
    && pass "its windows' classes name it: the desktop id, StartupWMClass, its program" \
    || fail "window classes: id '$(q "$W" org.gate.Editor)' class '$(q "$W" GateEditorWin)' exe '$(q "$W" gate-editor)'"
[ -z "$(q "$W" gate-launcher)" ] && [ "$(q "$W" gate-game-a)" = SG.LinuxApp.gate-game-a ] \
    && pass "a program several apps start names none of them (each game by its own id)" || fail "launcher: '$(q "$W" gate-launcher)' game '$(q "$W" gate-game-a)'"
jump=$("$WINE" "$T/probe.exe" jump "$ID" 2>/dev/null | tr -d '\r' | cut -f2 | tr '\n' '|')
case "$jump" in *"New Window|New Private Window|"*) pass "its .desktop actions are its jump list's tasks ($jump)" ;; *) fail "jump list: '$jump'" ;; esac
case "$jump" in *nameless*|*"(untitled)"*) fail "an action without a name is listed" ;; *) pass "an action without a name is not" ;; esac
if python3 -c 'import gi; gi.require_version("Gio", "2.0"); from gi.repository import Gio' 2>/dev/null; then
    n=$("$WINE" "$T/probe.exe" jump "$ID" 2>/dev/null | tr -d '\r' | awk -F'\t' '$2 == "New Private Window" { print NR - 1; exit }')
    "$WINE" "$T/probe.exe" invoke "$ID" "${n:-9}" >/dev/null 2>&1
    i=0; while [ ! -s "$T/action" ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
    grep -qx private "$T/action" 2>/dev/null && pass "a task starts the app's action (--action, GLib)" || fail "the task ran: '$(cat "$T/action" 2>/dev/null)'"
else
    echo "SKIP  starting an action (no python3-gi here)"
fi
# the files a Linux program opened (recently-used.xbel, GTK's): on its jump list's Recent
echo notes > "$T/notes.txt"; echo old > "$T/ancient.txt"
NOW=$(date -u +%Y-%m-%dT%H:%M:%S.000000Z)
cat > "$XDG_DATA_HOME/recently-used.xbel" <<XBEL
<?xml version="1.0" encoding="UTF-8"?>
<xbel version="1.0" xmlns:bookmark="http://www.freedesktop.org/standards/desktop-bookmarks" xmlns:mime="http://www.freedesktop.org/standards/shared-mime-info">
  <bookmark href="file://$T/notes.txt" added="$NOW" modified="$NOW" visited="$NOW">
    <info><metadata owner="http://freedesktop.org"><mime:mime-type type="text/plain"/>
      <bookmark:applications><bookmark:application name="Gate Editor" exec="&apos;gate-editor %u&apos;" modified="$NOW" count="1"/></bookmark:applications>
    </metadata></info>
  </bookmark>
  <bookmark href="file://$T/ancient.txt" added="2001-01-01T00:00:00Z" modified="2001-01-01T00:00:00Z" visited="2001-01-01T00:00:00Z">
    <info><metadata owner="http://freedesktop.org"><mime:mime-type type="text/plain"/>
      <bookmark:applications><bookmark:application name="gate-editor" exec="&apos;gate-editor %u&apos;" modified="2001-01-01T00:00:00Z" count="1"/></bookmark:applications>
    </metadata></info>
  </bookmark>
</xbel>
XBEL
"$WINE" "$EXE" --sync; "$WINESERVER" -w
jump=$("$WINE" "$T/probe.exe" jump "$ID" 2>/dev/null | tr -d '\r' | cut -f2 | tr '\n' '|')
case "$jump" in *notes*) pass "a file it opened (recently-used.xbel) is on its jump list's Recent ($jump)" ;; *) fail "recent: '$jump'" ;; esac
case "$jump" in *ancient*) fail "an entry from before the last thirty days was taken" ;; *) pass "one older than thirty days is not" ;; esac
rm "$T/share/applications/org.gate.Editor.desktop"
"$WINE" "$EXE" --sync; "$WINESERVER" -w
[ -z "$(q "$K" DisplayName)" ] && [ -z "$(q "$W" GateEditorWin)" ] && [ -z "$(q "$W" org.gate.Editor)" ] \
    && [ -z "$("$WINE" "$T/probe.exe" jump "$ID" 2>/dev/null | tr -d '\r')" ] \
    && pass "an app that goes: its ID, window classes and jump list go" || fail "after removal: name '$(q "$K" DisplayName)' class '$(q "$W" GateEditorWin)'"

echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
