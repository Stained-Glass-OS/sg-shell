#!/bin/sh
# Gate for Terminal's settings.json, user profiles and dragging pane dividers
# (sg-terminal, wt.exe), on a shell desktop under Xvfb, driven with the X
# mouse and keyboard and read back from the dump (SG_TERMINAL_DUMP), the file
# and screenshots:
#
#   - first run with the old registry settings (DefaultProfile, FontFace,
#     FontSize): settings.json is made at Windows Terminal's unpackaged place
#     (%LOCALAPPDATA%\Microsoft\Windows Terminal) with them as defaultProfile
#     (Command Prompt's GUID) and profiles.defaults.font, and the machine's
#     profiles as dynamic entries (guid, name, source)
#   - a settings.json of the user's (a comment, a trailing comma, keys we do
#     not know): a profile of their own (commandline, startingDirectory,
#     colorScheme of their own, font size) is the default -- its program runs
#     in its folder on its scheme's background (pixels) with its font; a
#     hidden profile is not in the + menu but `wt -p` still opens it; a
#     foreign dynamic profile (no generator here) is left out; a profile
#     without a GUID gets one written back, and the unknown keys stay
#   - actions: ctrl+alt+g opens a Command Prompt tab (newTab with a profile),
#     ctrl+shift+t is unbound (no new tab)
#   - Settings (Ctrl+,): a font size saved into profiles.defaults.font,
#     everything else in the file kept; a pane of the defaults' profile takes
#     it, the profile with its own size keeps its own
#   - editing the file renames a profile in the running window (reload)
#   - dragging the divider between two panes with the mouse: the left pane
#     narrows, the right widens, and each pane's program sees its new size
#     (ResizePseudoConsole); a divider between panes one above the other too
#
# Needs a wine-sg with 0131-0133 (SG_WINE_DIR), Xvfb, xdotool, ImageMagick,
# python3. Screenshots: build/terminal-settings-*.png. SG_TERMINAL_EXE runs
# another build (mutants: -DSG_MUTANT_NOUNKNOWN, -DSG_MUTANT_NODRAG,
# -DSG_MUTANT_NOUSERPROFILE).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_TERMINAL_EXE:-$HERE/build/sg-terminal64.exe}"
OUT="$HERE/build"
RC=0; DPY="${SG_TERMINAL_SETTINGS_DPY:-143}"; XP=""
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }

for need in Xvfb xdotool import convert python3; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
mkdir -p "$OUT"

T=$(mktemp -d /var/tmp/sg-terminal-settings.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; [ "${KEEP:-0}" = 1 ] && echo "kept $T" || rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
mkdir -p "$T/pfx"
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
unset WINEDLLOVERRIDES
cp "$EXE" "$T/sg-terminal64.exe"
winexe=$(wine winepath -w "$T/sg-terminal64.exe" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
for f in "$HERE/theme/50-sg-colors.reg" "$HERE/theme/52-sg-fonts.reg"; do
    wine reg import "$(wine winepath -w "$f" | tr -d '\r')" >/dev/null 2>&1
done
esc=$(printf '%s' "$winexe" | sed 's/\\/\\\\\\\\/g')
sed "s|Z:\\\\\\\\usr\\\\\\\\libexec\\\\\\\\stained-glass\\\\\\\\shell\\\\\\\\sg-terminal64.exe|$esc|g" \
    "$HERE/defaults/66-sg-terminal.reg" > "$T/terminal.reg"
wine reg import "$(wine winepath -w "$T/terminal.reg" | tr -d '\r')" >/dev/null 2>&1
x86_64-w64-mingw32-gcc -O2 -o "$T/pfx/drive_c/conprobe.exe" "$HERE/test/sg-terminal-probe.c" || fail "the probe did not build"
# the old registry settings, for the migration
reg 'HKCU\Software\Stained Glass\Terminal' /v DefaultProfile /d "Command Prompt"
reg 'HKCU\Software\Stained Glass\Terminal' /v FontFace /d "DejaVu Sans Mono"
reg 'HKCU\Software\Stained Glass\Terminal' /v FontSize /t REG_DWORD /d 13
mkdir -p "$T/pfx/drive_c/t/start"
wineserver -w
LAD=$(wine cmd /c echo %LOCALAPPDATA% 2>/dev/null | tr -d '\r')
JSON="$(wine winepath -u "$LAD" 2>/dev/null | tr -d '\r')/Microsoft/Windows Terminal/settings.json"

WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

DUMP="$T/terminal.dump"
export SG_TERMINAL_DUMP="$(wine winepath -w "$DUMP" | tr -d '\r')"
D() { sed -n "s/^$1 //p" "$DUMP" 2>/dev/null | head -1; }
wait_grep() { i=0; while ! grep -q "$1" "$DUMP" 2>/dev/null && [ $i -lt $(( ${2:-20} * 4 )) ]; do sleep 0.25; i=$((i + 1)); done; grep -q "$1" "$DUMP" 2>/dev/null; }
prow_has() { grep -q "^prow $1 [0-9]*: .*$2" "$DUMP" 2>/dev/null; }
wait_prow() { i=0; while ! prow_has "$1" "$2" && [ $i -lt $(( ${3:-20} * 4 )) ]; do sleep 0.25; i=$((i + 1)); done; prow_has "$1" "$2"; }
pane_line() { grep "^pane [0-9]* id=$1 " "$DUMP" 2>/dev/null | head -1; }
pane_field() { pane_line "$1" | sed "s/.* $2=\([^ :]*\).*/\1/"; }
pane_size() { r=$(pane_field "$1" size); echo "${r:-0x0}"; }
pane_rect() { r=$(pane_field "$1" rect | tr ',' ' '); echo "${r:-0 0 0 0}"; }
focus() { D focus; }
typ() { xdotool type --delay 60 "$1"; sleep 0.3; }
key() { xdotool key --delay 80 "$@"; sleep 0.6; }
shot() { import -window root "$OUT/terminal-settings-$1.png" 2>/dev/null; }
prompt() { wait_grep "^prow $1 [0-9]*: [A-Z]:\\\\.*>" "${2:-30}"; }
quit() {
    pkill -x 'sg-terminal64.e'
    i=0; while pgrep -x 'sg-terminal64.e' >/dev/null && [ $i -lt 20 ]; do sleep 0.3; i=$((i + 1)); done
    rm -f "$DUMP"
}
jq_() { python3 -c "import json,sys; d=json.load(open(sys.argv[1])); $1" "$JSON" 2>/dev/null; }

# --- 1. first run: the registry settings become settings.json -------------------------------------
wine start wt.exe >/dev/null 2>&1
if wait_grep '^panes 1$' 30; then pass "wt.exe opens"; else fail "no window: $(head -5 "$DUMP" 2>/dev/null)"; exit 1; fi
[ -f "$JSON" ] && pass "settings.json made at %LOCALAPPDATA%\\Microsoft\\Windows Terminal" || fail "no $JSON"
jq_ 'sys.exit(0 if d["defaultProfile"].lower()=="{0caa0dad-35be-5f56-a8ff-afceeeaa6101}" else 1)' \
    && pass "defaultProfile: Command Prompt's GUID, from the registry" || fail "defaultProfile: $(jq_ 'print(d.get("defaultProfile"))')"
jq_ 'f=d["profiles"]["defaults"]["font"]; sys.exit(0 if f["face"]=="DejaVu Sans Mono" and f["size"]==13 else 1)' \
    && pass "the registry's font as profiles.defaults.font" || fail "font: $(jq_ 'print(d["profiles"]["defaults"])')"
jq_ 'l=d["profiles"]["list"]; sys.exit(0 if any(p.get("guid","").lower()=="{0caa0dad-35be-5f56-a8ff-afceeeaa6101}" and p.get("source") for p in l) else 1)' \
    && pass "the machine's profiles as dynamic entries (guid, name, source)" || fail "list: $(jq_ 'print(d["profiles"]["list"])')"
grep -q '^tab 1 .*profile=Command Prompt' "$DUMP" && [ "$(D font | cut -d' ' -f1)" = 13 ] \
    && pass "the window uses them: Command Prompt, 13 pt" || fail "tab/font: $(grep '^tab 1' "$DUMP") / $(D font)"
quit

# --- 2. the user's settings.json ------------------------------------------------------------------
python3 - "$JSON" <<'EOF'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
echo = "{11111111-2222-3333-4444-555555555555}"
d["defaultProfile"] = echo
d["sgGateKeep"] = {"nested": [1, "two", True], "why": "a key Terminal does not know"}
d["profiles"]["list"] += [
    {"guid": echo, "name": "Echo Box", "commandline": "cmd.exe /k echo HELLO_FROM_PROFILE",
     "startingDirectory": "C:\\t\\start", "colorScheme": "Gate Red", "font": {"size": 16}, "sgProfileKeep": 7},
    {"name": "Hidden One", "commandline": "cmd.exe /k echo HIDDEN_ONE", "hidden": True},
    {"guid": "{b453ae62-4e3d-5e58-b989-0a998ec441b8}", "name": "Azure Cloud Shell", "source": "Windows.Terminal.Azure"},
]
d["schemes"] = [{"name": "Gate Red", "background": "#401010", "foreground": "#FFEEDD", "black": "#000000",
                 "red": "#FF0000", "green": "#00FF00", "yellow": "#FFFF00", "blue": "#0000FF", "purple": "#FF00FF",
                 "cyan": "#00FFFF", "white": "#FFFFFF", "cursorColor": "#FFFFFF", "selectionBackground": "#808080"}]
d["actions"] = [{"command": {"action": "newTab", "profile": "Command Prompt"}, "keys": "ctrl+alt+g"},
                {"command": "unbound", "keys": "ctrl+shift+t"}]
text = json.dumps(d, indent=4)
# JSONC as people write it: a comment and a trailing comma
text = "// my own settings\n" + text.replace('"why": "a key Terminal does not know"', '"why": "a key Terminal does not know",', 1)
open(p, "w").write(text)
EOF
wine start wt.exe >/dev/null 2>&1
wait_grep '^panes 1$' 30 || fail "no window with the user's settings"
[ -z "$(D settingserror)" ] && pass "the JSONC file (a comment, a trailing comma) is read" || fail "settings error: $(D settingserror)"
P=$(focus)
grep -q '^tab 1 .*profile=Echo Box' "$DUMP" && pass "the user's profile is the default" || fail "tab 1: $(grep '^tab 1' "$DUMP")"
wait_prow "$P" "HELLO_FROM_PROFILE" 20 && pass "its commandline runs (HELLO_FROM_PROFILE)" || fail "no HELLO_FROM_PROFILE: $(grep "^prow $P" "$DUMP" | head -3)"
prompt "$P" && typ "cd" && key Return
wait_prow "$P" 'C:\\t\\start$' 15 && pass "in its startingDirectory (C:\\t\\start)" || fail "cd: $(grep "^prow $P" "$DUMP" | tail -3)"
[ "$(pane_field "$P" bg)" = 401010 ] && [ "$(pane_field "$P" font)" = 16 ] && pass "its scheme (background 401010) and font size (16)" \
    || fail "pane bg=$(pane_field "$P" bg) font=$(pane_field "$P" font)"
sleep 0.6; shot user
set -- $(pane_rect "$P")
px=$(convert "$OUT/terminal-settings-user.png" -crop "1x1+$(( $3 - 30 ))+$(( $4 - 12 ))" -format '%[hex:u.p{0,0}]' info: 2>/dev/null)
[ "${px#401010}" != "$px" ] && pass "the pane is drawn on the scheme's background (pixel $px)" || fail "pane pixel $px"
grep -q '^profileinfo [0-9]* .*hidden=1' "$DUMP" && ! grep -q '^profile [0-9]* Azure' "$DUMP" \
    && pass "the hidden profile is known, the foreign dynamic one left out" || fail "profiles: $(grep '^profile' "$DUMP")"
jq_ 'sys.exit(0 if d["sgGateKeep"]["nested"]==[1,"two",True] and all(p.get("guid") for p in d["profiles"]["list"]) and [p for p in d["profiles"]["list"] if p["name"]=="Echo Box"][0].get("sgProfileKeep")==7 else 1)' \
    && pass "a GUID written for the profile without one; the unknown keys kept" || fail "rewrite: $(head -c 600 "$JSON")"
# the + menu's drop-down
set -- $(D menu); xdotool mousemove "$1" "$2" click 1
if wait_grep '^menuopen 1' 5; then
    sleep 0.5; shot menu
    grep -q '^menuitem [0-9]* Echo Box$' "$DUMP" && ! grep -q '^menuitem [0-9]* Hidden One$' "$DUMP" \
        && pass "the menu lists the user's profile, not the hidden one" || fail "menu: $(grep '^menuitem' "$DUMP")"
    key Escape
else fail "the menu did not open"; fi
wait_grep '^menuopen 0' 5
# actions
key ctrl+shift+t
[ "$(sed -n 's/^tabs \([0-9]*\).*/\1/p' "$DUMP")" = 1 ] && pass "ctrl+shift+t unbound: no new tab" || fail "tabs after ctrl+shift+t: $(grep '^tabs' "$DUMP")"
key ctrl+alt+g
if wait_grep '^tabs 2 ' 10 && grep -q '^tab 2 .*profile=Command Prompt' "$DUMP"; then pass "ctrl+alt+g: newTab with the Command Prompt profile"
else fail "ctrl+alt+g: $(grep '^tab' "$DUMP")"; fi
P2=$(focus)
[ "$(pane_field "$P2" font)" = 13 ] && pass "a Command Prompt pane has the defaults' font (13)" || fail "pane font $(pane_field "$P2" font)"

# --- 3. Settings (Ctrl+,) saves into the file ----------------------------------------------------------
key ctrl+comma
if wait_grep '^setctl 12 ' 5; then
    set -- $(sed -n 's/^setctl 12 //p' "$DUMP"); xdotool mousemove "$1" "$2" click 1; sleep 0.3
    key ctrl+a; typ "18"
    shot dialog
    set -- $(sed -n 's/^setctl 1 //p' "$DUMP"); xdotool mousemove "$1" "$2" click 1; sleep 1.5
    jq_ 'sys.exit(0 if d["profiles"]["defaults"]["font"]["size"]==18 else 1)' && pass "Settings saved the font size in profiles.defaults.font" \
        || fail "size in the file: $(jq_ 'print(d["profiles"]["defaults"])')"
    jq_ 'sys.exit(0 if d.get("sgGateKeep",{}).get("nested")==[1,"two",True] and any(p["name"]=="Echo Box" for p in d["profiles"]["list"]) and d["actions"] else 1)' \
        && pass "and kept everything else (unknown keys, profiles, actions)" || fail "the file after Settings: $(head -c 800 "$JSON")"
    i=0; while [ "$(pane_field "$P2" font)" != 18 ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
    EF=$(grep '^profileinfo .*guid={11111111-2222-3333-4444-555555555555}' "$DUMP" | sed 's/.* font=,\([0-9]*\) .*/\1/')
    [ "$(pane_field "$P2" font)" = 18 ] && [ "$EF" = 16 ] && pass "the defaults' pane takes 18, the profile with its own size keeps 16" \
        || fail "fonts after Settings: $(pane_field "$P2" font) / $EF"
else fail "no Settings dialog: $(grep -c setctl "$DUMP") $(ls -la "$DUMP") $(grep "^menuopen\|^actions\|^window" "$DUMP")"; cp "$DUMP" "$OUT/dump-dialog.txt"; shot nodialog; fi

# --- 4. an edit of the file is taken at once ------------------------------------------------------------
python3 -c "
import json,sys; p=sys.argv[1]; d=json.load(open(p))
[x for x in d['profiles']['list'] if x['name']=='Echo Box'][0]['name']='Echo Renamed'
json.dump(d, open(p,'w'), indent=4)" "$JSON"
wait_grep '^profile [0-9]* Echo Renamed' 6 && pass "editing settings.json renames the profile in the running window" || fail "no reload: $(grep '^profile ' "$DUMP")"

# --- 5. wt -p a hidden profile ------------------------------------------------------------------------
DUMP2="$T/terminal2.dump"
SG_TERMINAL_DUMP="$(wine winepath -w "$DUMP2" | tr -d '\r')" wine start wt.exe -p "Hidden One" >/dev/null 2>&1
i=0; while ! grep -q '^tab 1 ' "$DUMP2" 2>/dev/null && [ $i -lt 80 ]; do sleep 0.25; i=$((i + 1)); done
grep -q '^tab 1 .*profile=Hidden One' "$DUMP2" && pass "wt -p \"Hidden One\" opens the hidden profile" || fail "wt -p: $(grep '^tab 1' "$DUMP2")"
i=0; while ! grep -q '^prow .*HIDDEN_ONE' "$DUMP2" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.25; i=$((i + 1)); done
grep -q '^prow .*HIDDEN_ONE' "$DUMP2" && pass "and runs its command line" || fail "no HIDDEN_ONE"
# close the second window; keep the first
for pid in $(pgrep -x 'sg-terminal64.e'); do
    if tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | grep -q 'terminal2.dump'; then kill "$pid"; fi
done
sleep 1

# --- 6. dragging dividers --------------------------------------------------------------------------------
# the first window to the front again (the second one had it)
set -- $(pane_field "$P2" at | tr ',' ' '); xdotool mousemove "$1" "$2" click 1; sleep 0.5
key alt+shift+equal
if wait_grep '^panes 2$' 10; then
    PR=$(focus); PL=$P2
    prompt "$PR"
    set -- $(sed -n 's/^divider [0-9]* v //p' "$DUMP" | head -1)
    if [ $# -ge 2 ]; then
        dx=$1; dy=$2
        wl=$(pane_size "$PL"); wr=$(pane_size "$PR")
        xdotool mousemove "$dx" "$dy" sleep 0.3 mousedown 1 sleep 0.3 mousemove $((dx - 60)) "$dy" sleep 0.3 mousemove $((dx - 160)) "$dy" sleep 0.5 mouseup 1
        sleep 1
        wl2=$(pane_size "$PL"); wr2=$(pane_size "$PR")
        [ "${wl2%x*}" -lt "${wl%x*}" ] && [ "${wr2%x*}" -gt "${wr%x*}" ] && pass "dragging the divider 160 px left: left $wl -> $wl2, right $wr -> $wr2" \
            || fail "drag: left $wl -> $wl2, right $wr -> $wr2"
        shot dragged
        typ "c:\\conprobe.exe size"; key Return
        wait_prow "$PR" "size=$(pane_size "$PR")" 15 && pass "the right pane's program sees its new size ($(pane_size "$PR"))" \
            || fail "right probe: $(grep "^prow $PR .*size=" "$DUMP") vs $(pane_size "$PR")"
        key alt+Left
        typ "c:\\conprobe.exe size"; key Return
        wait_prow "$PL" "size=$(pane_size "$PL")" 15 && pass "the left pane's program sees its new size ($(pane_size "$PL"))" \
            || fail "left probe: $(grep "^prow $PL .*size=" "$DUMP") vs $(pane_size "$PL")"
        # a divider between panes one above the other
        key alt+shift+minus
        if wait_grep '^panes 3$' 10; then
            PB=$(focus)
            set -- $(sed -n 's/^divider [0-9]* h //p' "$DUMP" | head -1)
            hb=$(pane_size "$PB")
            xdotool mousemove "$1" "$2" sleep 0.3 mousedown 1 sleep 0.3 mousemove "$1" $(($2 - 50)) sleep 0.3 mousemove "$1" $(($2 - 120)) sleep 0.5 mouseup 1
            sleep 1
            hb2=$(pane_size "$PB")
            [ "${hb2#*x}" -gt "${hb#*x}" ] && pass "dragging a horizontal divider up grows the lower pane: $hb -> $hb2" || fail "horizontal drag: $hb -> $hb2"
            shot dragged2
        else fail "no third pane"; fi
    else fail "no divider in the dump"; fi
else fail "no split for the drag: $(grep "^tabs\|^pane " "$DUMP")"; shot nosplit; fi

quit
echo
[ $RC -eq 0 ] && echo "terminal-settings-check: all passed" || echo "terminal-settings-check: FAILED"
exit $RC
