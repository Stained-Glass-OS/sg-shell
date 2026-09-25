#!/bin/sh
# Gate for Settings (sg-settings): the Windows 10 Settings window on a shell
# desktop, opened through ms-settings: URIs and SystemSettings.exe (App Paths,
# defaults/65-sg-settings.reg), driven on the X mouse and keyboard (xdotool)
# and read back from the program's own dump (SG_SETTINGS_DUMP, rewritten
# after every page is shown), the registry and screenshots.
#
#   - ms-settings: names reach their pages (--resolve for the table, and for
#     real through ShellExecute: a second URI goes to the running window)
#   - About shows this PC's device name
#   - Background: clicking a picture makes it the desktop (registry, and the
#     desktop's pixels outside the window)
#   - Colors: clicking an accent writes AccentColor, and Settings itself
#     repaints in it (the navigation's selection bar)
#   - Sound: a stand-in sg-settingsctl's devices are listed; a slider moved
#     with the keyboard asks it for exactly that volume
#   - Privacy > Microphone: the switch writes the ConsentStore value
#   - search: typing in the navigation's box finds Bluetooth, Enter opens it
#   - Update: pending updates from sg-settingsctl
#   - Lock screen: a picture tile and the sign-in switch are published through
#     sg-settingsctl (the lock screen, drawn by the machine account, reads
#     only what is published)
#   - WINI=1: Win+I on the X keyboard opens it (a wine-sg with 0130)
#
# Screenshots: build/settings-*.png. Needs wine-sg, Xvfb, xdotool,
# ImageMagick; skips (77) without them. SG_SETTINGS_EXE runs another build
# (the mutation test); SG_WINE_DIR another Wine.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
OUT="$HERE/build"
RC=0; DPY="${SG_SETTINGS_DPY:-120}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import convert; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-settings-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; [ "${KEEP:-0}" = 1 ] && echo "kept $T" || rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
cp "$EXE" "$T/sg-settings64.exe"
winexe=$(wine winepath -w "$T/sg-settings64.exe" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
regq() { wine reg query "$1" /v "$2" 2>/dev/null | tr -d '\r' | awk -v v="$2" '$1 == v { $1 = ""; $2 = ""; sub(/^ +/, ""); print }'; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
for f in "$HERE/theme/50-sg-colors.reg" "$HERE/theme/52-sg-fonts.reg"; do
    wine reg import "$(wine winepath -w "$f" | tr -d '\r')" >/dev/null 2>&1
done
esc=$(printf '%s' "$winexe" | sed 's/\\/\\\\\\\\/g')
sed "s|Z:\\\\\\\\usr\\\\\\\\libexec\\\\\\\\stained-glass\\\\\\\\shell\\\\\\\\sg-settings64.exe|$esc|g" \
    "$HERE/defaults/65-sg-settings.reg" > "$T/settings.reg"
wine reg import "$(wine winepath -w "$T/settings.reg" | tr -d '\r')" >/dev/null 2>&1
# two pictures to choose from, in Windows' wallpaper folder
WP="$T/pfx/drive_c/windows/Web/Wallpaper"; mkdir -p "$WP"
convert -size 320x200 xc:'#E01010' "$WP/red.png"; convert -size 320x200 xc:'#10A020' "$WP/green.png"
wineserver -w
if regq 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\SystemSettings.exe' '(Default)' | grep -qF "$winexe" ||
   wine reg query 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\SystemSettings.exe' 2>/dev/null | tr -d '\r' | grep -qF "$winexe"
then pass "App Paths SystemSettings.exe names this build"; else fail "App Paths SystemSettings.exe was not registered"; fi

# the native half: a stand-in sg-settingsctl that answers as the real one and logs what it is asked
CTL="$T/sg-settingsctl"; LOG="$T/ctl.log"; : > "$LOG"
cat > "$CTL" <<EOF
#!/bin/sh
out=""; args=""
while [ \$# -gt 0 ]; do
    case "\$1" in --out) out="\$2"; shift 2 ;; *) args="\$args \$1"; shift ;; esac
done
echo "\${args# }" >> "$LOG"
set -- \$args
{
case "\$1" in
sound) [ \$# -eq 1 ] && printf 'SINK spk.0\tyes\t40\tno\tSpeakers (Stand-in)\nSOURCE mic.0\tyes\t70\tno\tMicrophone (Stand-in)\n'; echo OK ;;
nightlight) printf 'NIGHTLIGHT off\t4000\tyes\tno\nOK\n' ;;
display) echo 'ERROR unsupported the display is not the compositor' ;;
power) printf 'POWER 10\t0\tyes\tno\nOK\n' ;;
lockscreen) printf 'LOCKSCREEN yes\tyes\nOK\n' ;;
updates) printf 'UPDATE libfoo1\t1.0-1\t1.0-2\nUPDATE wine-sg\t10.0-37\t10.0-38\nSTAGED no\nOK\n' ;;
bluetooth) printf 'BLUETOOTH yes\nPOWERED yes\nDEVICE AA:BB:CC:DD:EE:01\tyes\tyes\tTravel Mouse\n'
    [ "\${2:-}" = scan ] && printf 'DEVICE AA:BB:CC:DD:EE:02\tno\tno\tKeyboard K2\n'
    echo OK ;;
*) echo 'ERROR invalid usage' ;;
esac
} > "\$out.part"
mv "\$out.part" "\$out"
EOF
chmod 755 "$CTL"
export SG_SETTINGSCTL="$CTL"

# --- the ms-settings: table, without a window --------------------------------------------
resolve() { wine "$T/sg-settings64.exe" --resolve "$1" 2>/dev/null | tr -d '\r' | sed -n 's/^page=//p'; }
for pair in "ms-settings:|Home" "ms-settings:display|Display" "ms-settings:network|Status" "ms-settings:sound|Sound" \
            "ms-settings:privacy-microphone|Microphone" "ms-settings:windowsupdate|Windows Update" \
            "ms-settings:appsfeatures|Apps & features" "ms-settings:defaultapps|Default apps" \
            "ms-settings:dateandtime|Date & time" "ms-settings:personalization-background|Background" \
            "ms-settings:colors|Colors" "ms-settings:about|About" "ms-settings:bluetooth|Bluetooth & other devices" \
            "ms-settings:network-proxy|Proxy" "ms-settings:speech|Speech Recognition" "ms-settings:lockscreen|Lock screen" \
            "ms-settings:display?activationSource=x|Display" "ms-settings:no-such-page|Home"; do
    uri=${pair%%|*}; want=${pair#*|}
    got=$(resolve "$uri")
    [ "$got" = "$want" ] && pass "$uri -> $want" || fail "$uri -> '$got', want '$want'"
done

WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

DUMP="$T/settings.dump"
export SG_SETTINGS_DUMP="$(wine winepath -w "$DUMP" | tr -d '\r')"
D() { tr -d '\r' < "$DUMP" 2>/dev/null | sed "s/^\xEF\xBB\xBF//" | sed -n "s/^$1 //p" | head -1; }
has() { tr -d '\r' < "$DUMP" 2>/dev/null | grep -qF -- "$1"; }
page_is() {
    i=0
    while [ "$(D page)" != "$1" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
    if [ "$(D page)" = "$1" ]; then pass "$2: the page is $1"; else fail "$2: the page is '$(D page)', want '$1'"; fi
}
ctl_at() {   # the screen centre of the page's control of class $1 whose text is $2
    tr -d '\r' < "$DUMP" | grep "^control $1 " | grep -F -- ": $2" | head -1 | sed -n 's/.* at=\([0-9-]*\),\([0-9-]*\).*/\1 \2/p'
}
click_at() { xdotool mousemove "$1" "$2" click 1; sleep 0.6; }
shot() { import -window root "$OUT/settings-$1.png" 2>/dev/null; }
pixel() { convert "$T/p.png" -format "%[fx:int(255*p{$1,$2}.r)] %[fx:int(255*p{$1,$2}.g)] %[fx:int(255*p{$1,$2}.b)]" info: 2>/dev/null; }
near() {  # "r g b" within 24 of "R G B"
    set -- $1 $2
    [ $(( ($1 - $4) * ($1 - $4) < 576 && ($2 - $5) * ($2 - $5) < 576 && ($3 - $6) * ($3 - $6) < 576 )) = 1 ]
}

wine start ms-settings:display >/dev/null 2>&1
i=0; while [ ! -s "$DUMP" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
[ -s "$DUMP" ] && pass "ms-settings:display starts Settings (ShellExecute of the URI)" || { fail "Settings did not start"; exit 1; }
page_is Display "ms-settings:display"
[ "$(D window)" = Settings ] && pass "the window is called Settings" || fail "window title '$(D window)'"
sleep 1
[ "$(D category)" = System ] && [ "$(D nav)" = shown ] && pass "the navigation shows System's pages" || fail "nav: $(D category) $(D nav)"
has "text Night light" && has "Display resolution" && pass "Display: night light and resolution" || fail "Display page content"
sleep 1; shot display

wine start ms-settings:about >/dev/null 2>&1
page_is About "a second URI goes to the running window"
i=0; while [ "$(pgrep -fc "^[A-Za-z]:.*${T##*/}.sg-settings64.exe" 2>/dev/null)" -gt 1 ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
[ "$(pgrep -fc "^[A-Za-z]:.*${T##*/}.sg-settings64.exe" 2>/dev/null)" -le 1 ] && pass "the second start handed over and left: one Settings process" || fail "more than one Settings process"
host=$(wine hostname 2>/dev/null | tr -d '\r')
if tr -d '\r' < "$DUMP" | grep -qix "text $host"; then pass "About shows the device name ($host)"; else fail "About does not show '$host'"; fi
has "text Device name" && has "text Processor" && pass "About lists the device specifications" || fail "About's specifications"
has ": Rename this PC" && pass "About offers Rename this PC" || fail "no Rename this PC button"
sleep 0.5; shot about

# --- Background: a picture becomes the desktop ------------------------------------------------
wine start ms-settings:personalization-background >/dev/null 2>&1
page_is Background "ms-settings:personalization-background"
sleep 0.5
# a fresh profile has a solid colour: choose Picture in the Background list
if tr -d '\r' < "$DUMP" | grep -q '^control ComboBox .*: Solid color'; then
    set -- $(ctl_at ComboBox 'Solid color')
    click_at "$1" "$2"; xdotool key Up Return; sleep 2
fi
has ": Picture" && pass "the Background list switches to Picture" || fail "Background is not Picture"
set -- $(ctl_at SgCplTile red.png)
if [ $# -eq 2 ]; then
    click_at "$1" "$2"
    i=0; while ! regq 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Wallpapers' BackgroundHistoryPath0 | grep -qi 'red.png' && [ $i -lt 30 ]; do sleep 0.5; i=$((i + 1)); done
    regq 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Wallpapers' BackgroundHistoryPath0 | grep -qi 'red.png' \
        && pass "clicking red.png makes it the background (BackgroundHistoryPath0)" || fail "background not recorded"
    regq 'HKCU\Control Panel\Desktop' Wallpaper | grep -qi 'TranscodedWallpaper' && pass "Wallpaper is the transcoded picture" || fail "Wallpaper: $(regq 'HKCU\Control Panel\Desktop' Wallpaper)"
    sleep 1.5
    import -window root "$T/p.png" 2>/dev/null
    set -- $(D rect)
    px=$(( $3 + 20 )); [ $px -gt 1015 ] && px=$(( $1 - 20 ))
    if near "$(pixel $px 300)" "224 16 16"; then pass "the desktop shows the red picture outside the window ($px,300)"
    else fail "desktop pixel at $px,300 is $(pixel $px 300), not red"; fi
    shot background
else fail "no red.png tile on the Background page"; tr -d '\r' < "$DUMP" | grep '^control' | head; shot background; fi

# --- Lock screen: the choice is published for the lock screen -----------------------------------
wine start ms-settings:lockscreen >/dev/null 2>&1
page_is "Lock screen" "ms-settings:lockscreen"
sleep 0.5
set -- $(ctl_at SgCplTile green.png)
if [ $# -eq 2 ]; then
    click_at "$1" "$2"
    i=0; while ! grep -q '^lockscreen picture .*/Web/Wallpaper/green.png$' "$LOG" && [ $i -lt 30 ]; do sleep 0.5; i=$((i + 1)); done
    grep -q '^lockscreen picture .*/Web/Wallpaper/green.png$' "$LOG" \
        && pass "a lock-screen picture is published by its Unix path (sg-settingsctl lockscreen picture)" \
        || fail "the lock-screen picture was not published: $(grep lockscreen "$LOG" | tail -1)"
    regq 'HKCU\Software\Stained Glass\LockScreen' Picture | grep -qi 'green.png' && pass "and recorded (LockScreen\\Picture)" \
        || fail "LockScreen\\Picture: $(regq 'HKCU\Software\Stained Glass\LockScreen' Picture)"
else fail "no green.png tile on the Lock screen page"; fi
# the page's only switch: "Show lock screen background picture on the sign-in screen"
set -- $(tr -d '\r' < "$DUMP" | grep '^control SgSetCtl ' | head -1 | sed -n 's/.* at=\([0-9]*\),\([0-9]*\).*/\1 \2/p')
if [ $# -eq 2 ]; then
    click_at "$1" "$2"
    i=0; while ! grep -q '^lockscreen signin no$' "$LOG" && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
    grep -q '^lockscreen signin no$' "$LOG" && pass "the sign-in switch is published (lockscreen signin no)" \
        || fail "the sign-in switch was not published"
else fail "no sign-in switch on the Lock screen page"; fi
sleep 0.5; shot lockscreen

# --- Colors: an accent is written and Settings repaints in it ---------------------------------
wine start ms-settings:colors >/dev/null 2>&1
page_is Colors "ms-settings:colors"
sleep 0.5
# the accent tiles are below the fold: wheel down over the page until the dump says they are on screen
set -- $(D rect)
xdotool mousemove $(( ($1 + $3) / 2 + 150 )) 400
for n in 1 2 3 4 5 6 7 8; do xdotool click 5; sleep 0.3; done
sleep 0.5
set -- $(ctl_at SgCplTile '#107C41')
if [ $# -eq 2 ]; then
    click_at "$1" "$2"
    i=0; while [ "$(regq 'HKCU\Software\Microsoft\Windows\DWM' AccentColor)" != 0xff417c10 ] && [ $i -lt 20 ]; do sleep 0.3; i=$((i + 1)); done
    a=$(regq 'HKCU\Software\Microsoft\Windows\DWM' AccentColor)
    [ "$a" = 0xff417c10 ] && pass "the accent #107C41 is written (AccentColor $a)" || fail "AccentColor is '$a', want 0xff417c10"
    i=0; while [ "$(D accent)" != 107C41 ] && [ $i -lt 20 ]; do sleep 0.3; i=$((i + 1)); done
    [ "$(D accent)" = 107C41 ] && pass "Settings takes the new accent (WM_SETTINGCHANGE)" || fail "Settings' accent is $(D accent)"
    sleep 1
    import -window root "$T/p.png" 2>/dev/null
    set -- $(D accentbar)
    if [ $# -eq 2 ] && near "$(pixel "$1" "$2")" "16 124 65"; then pass "the navigation's selection bar is drawn in the new accent"
    else fail "selection bar pixel $(pixel "${1:-0}" "${2:-0}") is not #107C41"; fi
    shot colors
else fail "no #107C41 accent tile"; fi

# --- Sound, through sg-settingsctl ----------------------------------------------------------------
wine start ms-settings:sound >/dev/null 2>&1
page_is Sound "ms-settings:sound"
has ": Speakers (Stand-in)" && has ": Microphone (Stand-in)" && pass "Sound lists sg-settingsctl's output and input" || fail "Sound devices not listed"
set -- $(tr -d '\r' < "$DUMP" | grep '^control msctls_trackbar32 ' | head -1 | sed -n 's/.* state=\([0-9]*\) at=\([0-9]*\),\([0-9]*\).*/\1 \2 \3/p')
if [ $# -eq 3 ] && [ "$1" = 40 ]; then
    pass "the master volume slider is at 40"
    : > "$LOG"
    click_at "$2" "$3"
    # the click moved the thumb somewhere; Home then one step right is exactly 1
    xdotool key Home; sleep 0.8; xdotool key Right; sleep 1.2
    tail -1 "$LOG" | grep -q '^sound volume sink spk.0 1$' && pass "the slider asks sg-settingsctl for that volume" || fail "sg-settingsctl heard: $(cat "$LOG")"
else fail "no master volume slider at 40 ($*)"; fi
shot sound

# --- Privacy > Microphone ---------------------------------------------------------------------------
wine start ms-settings:privacy-microphone >/dev/null 2>&1
page_is Microphone "ms-settings:privacy-microphone"
set -- $(tr -d '\r' < "$DUMP" | grep '^control SgSetCtl ' | head -1 | sed -n 's/.* at=\([0-9]*\),\([0-9]*\).*/\1 \2/p')
if [ $# -eq 2 ]; then
    click_at "$(( $1 - 30 ))" "$2"
    v=$(regq 'HKCU\Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\microphone' Value)
    [ "$v" = Deny ] && pass "turning apps' microphone access off writes ConsentStore Deny" || fail "ConsentStore microphone Value is '$v'"
    shot microphone
else fail "no microphone switch"; fi

# --- search ---------------------------------------------------------------------------------------------
set -- $(D navsearch)
click_at "$1" "$2"
xdotool type --delay 80 bluetooth; sleep 1
page_is "Search results" "typing in the search box"
has "Bluetooth & other devices" && pass "search finds Bluetooth & other devices" || fail "search results: $(tr -d '\r' < "$DUMP" | grep '^text' | head -5)"
shot search
xdotool key Return
page_is "Bluetooth & other devices" "Enter opens the first result"
has "text Travel Mouse" && pass "Bluetooth lists sg-settingsctl's paired device" || fail "no Travel Mouse"
shot bluetooth
set -- $(ctl_at Button 'Add Bluetooth or other device')
if [ $# -eq 2 ]; then
    : > "$LOG"; click_at "$1" "$2"; sleep 1.5
    grep -q '^bluetooth scan 8' "$LOG" && has "text Keyboard K2" && pass "Add a device searches and lists what it found" || fail "scan: $(cat "$LOG")"
    set -- $(ctl_at SgCplLink Pair)
    if [ $# -eq 2 ]; then click_at "$1" "$2"; sleep 1
        grep -q '^bluetooth pair AA:BB:CC:DD:EE:02' "$LOG" && pass "Pair pairs that device" || fail "pair: $(cat "$LOG")"
    else fail "no Pair link"; fi
    shot bluetooth-add
else fail "no Add Bluetooth or other device button"; fi

wine start ms-settings:windowsupdate >/dev/null 2>&1
page_is "Windows Update" "ms-settings:windowsupdate"
has "text Updates available" && has "wine-sg 10.0-38 (installed: 10.0-37)" && pass "Update lists the pending updates" || fail "Update page: $(tr -d '\r' < "$DUMP" | grep '^text' | head -8)"
shot update

wine start SystemSettings.exe >/dev/null 2>&1
page_is Home "SystemSettings.exe (App Paths)"
[ "$(D nav)" = hidden ] && pass "Home has no navigation pane" || fail "nav on Home: $(D nav)"
has ": System" && has ": Update & Security" && pass "Home shows the categories" || fail "Home tiles"
sleep 0.5; shot home

wine start ms-settings:no-such-page >/dev/null 2>&1
wine start ms-settings:dateandtime >/dev/null 2>&1
page_is "Date & time" "ms-settings:dateandtime"
has "text Set time automatically" && pass "Date & time: the clock, time synchronisation and the zone" || fail "Date & time page"
sleep 0.5; shot datetime
wine start ms-settings:no-such-page >/dev/null 2>&1
page_is Home "an unknown ms-settings: name"

# every page builds and shows (a crash or a hang in any of them turns this red)
for pair in "notifications|Notifications & actions" "powersleep|Power & sleep" "storagesense|Storage" \
            "multitasking|Multitasking" "mousetouchpad|Mouse" "typing|Typing" "network-status|Status" "network-wifi|Wi-Fi" \
            "network-ethernet|Ethernet" "network-proxy|Proxy" "lockscreen|Lock screen" "themes|Themes" \
            "personalization-start|Start" "taskbar|Taskbar" "appsfeatures|Apps & features" "defaultapps|Default apps" \
            "startupapps|Startup" "yourinfo|Your info" "signinoptions|Sign-in options" "otherusers|Other users" \
            "regionformatting|Region" "speech|Speech Recognition" "easeofaccess-display|Display" \
            "easeofaccess-keyboard|Keyboard" "easeofaccess-mouse|Mouse pointer" "privacy|General" "privacy-webcam|Camera" \
            "privacy-location|Location" "recovery|Recovery"; do
    uri=${pair%%|*}; want=${pair#*|}
    wine start "ms-settings:$uri" >/dev/null 2>&1
    page_is "$want" "ms-settings:$uri"
    sleep 0.4; shot "page-$uri"
done
pgrep -f "^[A-Za-z]:.*${T##*/}.sg-settings64.exe" >/dev/null && pass "Settings is still running after every page" || fail "Settings is gone"
wine start ms-settings:regionformatting >/dev/null 2>&1
page_is Region "ms-settings:regionformatting"
has ": English (United States)" && has ": United States" && pass "Region shows this user's format and country" \
    || fail "Region: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"

# --- Taskbar: what the page shows and writes (the taskbar itself: wine-sg's test/taskbar-gate.sh) --
wine reg add 'HKCU\Software\Stained Glass\Taskbar' /v Position /t REG_DWORD /d 1 /f >/dev/null 2>&1
wine reg add 'HKCU\Software\Microsoft\Windows\CurrentVersion\Search' /v SearchboxTaskbarMode /t REG_DWORD /d 2 /f >/dev/null 2>&1
wine start ms-settings:taskbar >/dev/null 2>&1
page_is Taskbar "ms-settings:taskbar (again)"
sleep 0.5
has ": Top" && has ": Show search box" && has ": Never" \
    && pass "Taskbar shows the location, search and combining as stored" || fail "Taskbar page: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"
# the sixth switch: "Show Task View button"
set -- $(tr -d '\r' < "$DUMP" | grep '^control SgSetCtl ' | sed -n '6s/.* at=\([0-9]*\),\([0-9]*\).*/\1 \2/p')
if [ $# -eq 2 ]; then
    click_at "$1" "$2"
    regq 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' ShowTaskViewButton | grep -q '0x0' \
        && pass "the Task View switch writes Explorer\\Advanced ShowTaskViewButton" || fail "ShowTaskViewButton: $(regq 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' ShowTaskViewButton)"
else fail "no Task View switch on the Taskbar page"; fi
sleep 0.5; shot taskbar-settings

if [ "${WINI:-0}" = 1 ]; then
    wine start ms-settings:about >/dev/null 2>&1; page_is About "(before Win+I)"
    xdotool key super+i; sleep 2
    page_is Home "Win+I"
fi

[ $RC = 0 ] && echo "settings-check: PASS" || echo "settings-check: FAIL"
exit $RC
