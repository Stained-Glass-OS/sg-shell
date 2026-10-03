#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
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
# the machine's zone as Debian's installer leaves UTC: /etc/localtime -> zoneinfo/UTC, itself a link to Etc/UTC
ln -s /usr/share/zoneinfo/UTC "$T/localtime"
export SG_SETTINGS_LOCALTIME="$T/localtime"
# Stained Glass's own version (os-release), not the Windows version Wine reports
printf 'NAME="Stained Glass OS"\nID=stained-glass\nID_LIKE=debian\nVERSION_ID=0.9\nIMAGE_VERSION=20260926-abc1234\n' > "$T/os-release"
export SG_OS_RELEASE="$T/os-release"
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
updates) st=\$(cat "$T/upd-state" 2>/dev/null)
    [ "\${2:-}" = progress ] || printf 'UPDATE libfoo1\t1.0-1\t1.0-2\nUPDATE wine-sg\t10.0-37\t10.0-38\n'
    case "\$st" in
    downloading) printf 'DOWNLOAD libfoo1\t1000\t1000\nDOWNLOAD wine-sg\t4000000\t1000000\nDOWNLOADING yes\nSTAGED no\n' ;;
    staged) printf 'DOWNLOAD libfoo1\t1000\t1000\nDOWNLOAD wine-sg\t4000000\t4000000\nDOWNLOADING no\nSTAGED yes\n' ;;
    *) printf 'STAGED no\n' ;;
    esac; echo OK ;;
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
            "ms-settings:privacy-microphone|Microphone" "ms-settings:windowsupdate|Updates" \
            "ms-settings:appsfeatures|Apps & features" "ms-settings:defaultapps|Default apps" \
            "ms-settings:dateandtime|Date & time" "ms-settings:personalization-background|Background" \
            "ms-settings:colors|Colors" "ms-settings:about|About" "ms-settings:bluetooth|Bluetooth & other devices" \
            "ms-settings:network-proxy|Proxy" "ms-settings:speech|Speech Recognition" "ms-settings:lockscreen|Lock screen" \
            "ms-settings:windowsdefender|Virus & threat protection" \
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
if tr -d '\r' < "$DUMP" | grep -qx "text 0.9 (build 20260926-abc1234)"; then pass "About's version is Stained Glass's (os-release), not Windows'"
else fail "About's version: $(tr -d '\r' < "$DUMP" | grep -A1 '^text Version$' | tail -1)"; fi
tr -d '\r' < "$DUMP" | grep -q '22H2\|19045' && fail "About still shows Windows' version" || pass "no Windows version on About"
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
    # the moving backgrounds are kinds of picture in the same list (David
    # 2026-10-03: a menu of their own overrode Picture / Solid color)
    set -- $(ctl_at ComboBox 'Picture')
    if [ $# -eq 2 ]; then
        click_at "$1" "$2"; xdotool key Down Down Down Return; sleep 2
        [ "$(regq 'HKCU\Software\Stained Glass\Effects' AnimatedBackground)" = 0x2 ] && has ": Living glass" \
            && tr -d '\r' < "$DUMP" | grep -q 'SgCplTile.*red.png' \
            && pass "Living glass is chosen in the Background list, the pictures still offered" \
            || fail "Living glass from the list: $(regq 'HKCU\Software\Stained Glass\Effects' AnimatedBackground)"
        set -- $(ctl_at ComboBox 'Living glass')
        [ $# -eq 2 ] && { click_at "$1" "$2"; xdotool key Up Up Up Return; sleep 2; }
        [ "$(regq 'HKCU\Software\Stained Glass\Effects' AnimatedBackground)" = 0x0 ] && has ": Picture" \
            && pass "and Picture again turns it off" || fail "back to Picture: $(regq 'HKCU\Software\Stained Glass\Effects' AnimatedBackground)"
    else fail "no Background list showing Picture"; fi
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
xdotool mousemove $(( $3 - 60 )) 400   # clear of the page's boxes, which take the wheel themselves
for n in 1 2 3 4 5 6 7 8 9 10; do xdotool click 5; sleep 0.3; done
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

# --- Dark app mode: accent text is a lighter shade, readable on #202020 (QA B23) ---------------
[ "$(D link)" = 107C41 ] && pass "light mode: links are the accent itself" || fail "light-mode link colour $(D link)"
wine "$T/sg-settings64.exe" --set mode apps dark >/dev/null 2>&1
i=0; while [ "$(D link)" = 107C41 ] && [ $i -lt 30 ]; do sleep 0.3; i=$((i + 1)); done
l=$(D link); lum=$(printf '%d %d %d' "0x${l%????}" "0x$(echo "$l" | cut -c3-4)" "0x${l#????}" | awk '{ print int(0.299*$1 + 0.587*$2 + 0.114*$3) }')
[ "${lum:-0}" -ge 140 ] && pass "dark mode: links and accent text are a light shade of the accent ($l, luminance $lum)" \
    || fail "dark mode: link colour $l (luminance $lum) is too dark for #202020"
[ "$(D accent)" = 107C41 ] && pass "and the accent itself (fills) is unchanged" || fail "dark mode changed the accent to $(D accent)"
shot colors-dark
wine "$T/sg-settings64.exe" --set mode apps light >/dev/null 2>&1
i=0; while [ "$(D link)" != 107C41 ] && [ $i -lt 30 ]; do sleep 0.3; i=$((i + 1)); done

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
    # (the volume chime's "sound chime" follows each change: the last volume asked)
    grep '^sound volume ' "$LOG" | tail -1 | grep -q '^sound volume sink spk.0 1$' && pass "the slider asks sg-settingsctl for that volume" || fail "sg-settingsctl heard: $(cat "$LOG")"
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
page_is "Updates" "ms-settings:windowsupdate"
has "text Updates available" && has "wine-sg 10.0-38 (installed: 10.0-37)" && pass "Update lists the pending updates" || fail "Update page: $(tr -d '\r' < "$DUMP" | grep '^text' | head -8)"
shot update
# David: "it should show the download progress of each update. and when they
# are ready to be installed offer a reboot button"
echo downloading > "$T/upd-state"
wine start ms-settings:windowsupdate >/dev/null 2>&1; sleep 2.5
has "text Downloading updates" && has "text Downloading - 25% of 3.8 MB" && has "text Downloaded" \
    && pass "Update shows each update's download" || fail "downloading: $(tr -d '\r' < "$DUMP" | grep '^text' | head -12)"
shot update-downloading
echo staged > "$T/upd-state"; sleep 3.5       # the page's own timer notices the end
has "text Restart required" && [ -n "$(ctl_at Button 'Restart now')" ] && has "text Ready to install" \
    && pass "a finished download offers Restart now, by itself" || fail "staged: $(tr -d '\r' < "$DUMP" | grep -E '^(text|control)' | head -12)"
rm -f "$T/upd-state"

wine start SystemSettings.exe >/dev/null 2>&1
page_is Home "SystemSettings.exe (App Paths)"
[ "$(D nav)" = hidden ] && pass "Home has no navigation pane" || fail "nav on Home: $(D nav)"
has ": System" && has ": Update & Security" && pass "Home shows the categories" || fail "Home tiles"
sleep 0.5; shot home

wine start ms-settings:no-such-page >/dev/null 2>&1
wine start ms-settings:dateandtime >/dev/null 2>&1
page_is "Date & time" "ms-settings:dateandtime"
has "text Set time automatically" && pass "Date & time: the clock, time synchronisation and the zone" || fail "Date & time page"
# the zone box shows the zone that is set (UTC -- Debian's Etc/UTC -- was an empty box), and the
# zone's line has its offset once ("(UTC+00:00) (UTC+00:00) Monrovia" was the 2026-09-26 ISO's)
tr -d '\r' < "$DUMP" | grep -q '^control ComboBox .*: UTC$' && pass "the time zone box shows UTC (Etc/UTC)" \
    || fail "zone box: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox' | head -2)"
zl=$(tr -d '\r' < "$DUMP" | grep '^text (UTC' | head -1)
[ -n "$zl" ] && [ "$(printf '%s' "$zl" | grep -o '(UTC' | wc -l)" = 1 ] && pass "the zone's line has its offset once ($zl)" \
    || fail "zone line: '$zl'"
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

# --- the looks: Classic and Rounded, and the parts they set, each choosable after ------------------
wine "$T/sg-settings64.exe" --set look rounded 2>/dev/null | tr -d '\r' | grep -q '^OK' && pass "--set look rounded" || fail "--set look rounded failed"
ADVK='HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
[ "$(regq 'HKCU\Software\Stained Glass\Style' Rounded)" = 0x1 ] && [ "$(regq "$ADVK" TaskbarAl)" = 0x1 ] \
    && [ "$(regq 'HKCU\Software\Stained Glass\Start' Centered)" = 0x1 ] && [ "$(regq "$ADVK" TaskbarGlomLevel)" = 0x0 ] \
    && [ "$(regq 'HKCU\Software\Stained Glass\Taskbar' ShowDesktops)" = 0x0 ] \
    && pass "Rounded: round corners, a centred taskbar of icons, centred Start, no desktops pager" \
    || fail "Rounded look: style $(regq 'HKCU\Software\Stained Glass\Style' Rounded) al $(regq "$ADVK" TaskbarAl) start $(regq 'HKCU\Software\Stained Glass\Start' Centered) glom $(regq "$ADVK" TaskbarGlomLevel) desktops $(regq 'HKCU\Software\Stained Glass\Taskbar' ShowDesktops)"
PINDIR="$WINEPREFIX/drive_c/users/$(id -un)/AppData/Roaming/Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar"
[ -f "$PINDIR/File Explorer.lnk" ] && [ -f "$PINDIR/Settings.lnk" ] && wine reg query 'HKCU\Software\Stained Glass\Taskbar' /v PinOrder 2>/dev/null | grep -q 'File Explorer.lnk' \
    && pass "and File Explorer and Settings pinned to the taskbar, in order" || fail "pins: $(ls "$PINDIR" 2>&1 | tr '\n' ' ')"
wine start ms-settings:themes >/dev/null 2>&1
page_is Themes "ms-settings:themes (look)"
sleep 0.5
has ": Rounded" && ! has "a centered taskbar an" && pass "Themes shows the Rounded look (a short name that fits)" || fail "Themes look: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"
wine start ms-settings:colors >/dev/null 2>&1
page_is Colors "ms-settings:colors (style)"
sleep 0.5
has ": Rounded: round corners" && pass "Colors shows the Rounded window style" || fail "Colors style: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"
wine "$T/sg-settings64.exe" --set style classic >/dev/null 2>&1
wine start ms-settings:themes >/dev/null 2>&1
page_is Themes "ms-settings:themes (mixed)"
sleep 0.5
tr -d '\r' < "$DUMP" | grep -q '^control ComboBox .*: Custom$' && pass "square windows with the centred taskbar: the look is Custom" || fail "mixed look: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"
wine reg add 'HKCU\Software\Stained Glass\Taskbar' /v Color /t REG_DWORD /d 3 /f >/dev/null 2>&1
wine start ms-settings:taskbar >/dev/null 2>&1
page_is Taskbar "ms-settings:taskbar (color)"
sleep 0.5
has ": Light blue" && has "Show virtual desktops on the taskbar" && pass "Taskbar: its color and the virtual desktops switch" \
    || fail "Taskbar color: $(tr -d '\r' < "$DUMP" | grep '^control' | grep -i 'combo\|desktops' | head -8)"
wine "$T/sg-settings64.exe" --set look classic >/dev/null 2>&1
# the virtual-desktops pager is off by default in every look (David 2026-09-29)
[ "$(regq 'HKCU\Software\Stained Glass\Style' Rounded)" = 0x0 ] && [ "$(regq "$ADVK" TaskbarAl)" = 0x0 ] && [ "$(regq 'HKCU\Software\Stained Glass\Start' Centered)" = 0x0 ] \
    && [ "$(regq 'HKCU\Software\Stained Glass\Taskbar' ShowDesktops)" = 0x0 ] && pass "Classic sets it all back, desktops pager still off" || fail "Classic look: desktops $(regq 'HKCU\Software\Stained Glass\Taskbar' ShowDesktops)"

# --- the Horizon and Glass looks: the taskbar of older desktops (wine-sg 0600) ----------------------
TBS() { regq 'HKCU\Software\Stained Glass\Taskbar' Style; }
[ "$(TBS)" = 0x0 ] && pass "Classic: the flat taskbar (Taskbar Style 0)" || fail "Classic taskbar style: $(TBS)"
wine "$T/sg-settings64.exe" --set look horizon 2>/dev/null | tr -d '\r' | grep -q '^OK' && [ "$(TBS)" = 0x1 ] \
    && [ "$(regq 'HKCU\Software\Stained Glass\Style' Rounded)" = 0x0 ] && [ "$(regq "$ADVK" TaskbarAl)" = 0x0 ] \
    && [ "$(regq "$ADVK" TaskbarGlomLevel)" = 0x1 ] && [ "$(regq "$ADVK" ShowTaskViewButton)" = 0x0 ] \
    && [ "$(regq 'HKCU\Software\Stained Glass\Start' Centered)" = 0x0 ] \
    && pass "--set look horizon: Taskbar Style 1, square windows, labelled buttons at the left, no Task View" \
    || fail "Horizon look: style $(TBS) rounded $(regq 'HKCU\Software\Stained Glass\Style' Rounded) glom $(regq "$ADVK" TaskbarGlomLevel) taskview $(regq "$ADVK" ShowTaskViewButton)"
wine start ms-settings:themes >/dev/null 2>&1
page_is Themes "ms-settings:themes (horizon)"
sleep 0.5
has ": Horizon" && pass "Themes shows the Horizon look" || fail "Themes horizon: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"
# the look's window frames (wine-sg 0742) and that era's title bar sizes
WM='HKCU\Control Panel\Desktop\WindowMetrics'
cfweight() { wine reg query "$WM" /v CaptionFont 2>/dev/null | tr -d '\r' | sed -n 's/.*REG_BINARY *//p' | cut -c33-36; }
grep -qx 'shadow=horizon' "${XDG_CONFIG_HOME:-$HOME/.config}/stained-glass/effects.conf" 2>/dev/null \
    && pass "Horizon: the compositor's shadows are that era's (shadow=horizon)" || fail "Horizon shadow: $(cat "${XDG_CONFIG_HOME:-$HOME/.config}/stained-glass/effects.conf" 2>&1 | tr '\n' ' ')"
[ "$(regq 'HKCU\Software\Stained Glass\Style' Frame)" = 0x1 ] && [ "$(regq "$WM" CaptionHeight)" = 25 ] && [ "$(regq "$WM" BorderWidth)" = 2 ] \
    && [ "$(cfweight)" = BC02 ] \
    && pass "Horizon: its window frames (Style Frame 1), 25 px bold title bars, 2 px borders" \
    || fail "Horizon frames: frame $(regq 'HKCU\Software\Stained Glass\Style' Frame) caption $(regq "$WM" CaptionHeight) border $(regq "$WM" BorderWidth) weight $(cfweight)"
wine start ms-settings:colors >/dev/null 2>&1
page_is Colors "ms-settings:colors (horizon frame)"
sleep 0.5
has ": Horizon: a blue title bar" && pass "Colors shows the Horizon window style" || fail "Colors horizon style: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"
wine start ms-settings:taskbar >/dev/null 2>&1
page_is Taskbar "ms-settings:taskbar (style)"
sleep 0.5
has ": Horizon: bright blue" && pass "Taskbar shows its style: Horizon" || fail "Taskbar style: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"
wine "$T/sg-settings64.exe" --set look glass 2>/dev/null | tr -d '\r' | grep -q '^OK' && [ "$(TBS)" = 0x2 ] \
    && [ "$(regq "$ADVK" TaskbarGlomLevel)" = 0x0 ] && [ "$(regq 'HKCU\Software\Stained Glass\Style' Rounded)" = 0x0 ] \
    && pass "--set look glass: Taskbar Style 2, combined icon buttons, square windows" \
    || fail "Glass look: style $(TBS) glom $(regq "$ADVK" TaskbarGlomLevel)"
[ "$(regq 'HKCU\Software\Stained Glass\Style' Frame)" = 0x2 ] && [ "$(regq "$WM" CaptionHeight)" = 21 ] && [ "$(regq "$WM" BorderWidth)" = 7 ] \
    && [ "$(cfweight)" = 9001 ] \
    && pass "Glass: its window frames (Style Frame 2), thick glass borders, a regular title" \
    || fail "Glass frames: frame $(regq 'HKCU\Software\Stained Glass\Style' Frame) caption $(regq "$WM" CaptionHeight) border $(regq "$WM" BorderWidth) weight $(cfweight)"
wine "$T/sg-settings64.exe" --dump personalization 2>/dev/null | tr -d '\r' | grep -qx 'taskbar.style=glass' \
    && pass "--dump personalization: taskbar.style=glass" || fail "dump: $(wine "$T/sg-settings64.exe" --dump personalization 2>/dev/null | tr -d '\r' | grep taskbar)"
wine start ms-settings:themes >/dev/null 2>&1
page_is Themes "ms-settings:themes (glass)"
sleep 0.5
has ": Glass" && pass "Themes shows the Glass look" || fail "Themes glass: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"
wine "$T/sg-settings64.exe" --set look classic >/dev/null 2>&1
[ "$(TBS)" = 0x0 ] && [ "$(regq "$ADVK" ShowTaskViewButton)" = 0x1 ] && [ "$(regq "$ADVK" TaskbarGlomLevel)" = 0x2 ] \
    && pass "Classic again: the flat taskbar, Task View, labelled buttons" || fail "Classic after glass: style $(TBS) taskview $(regq "$ADVK" ShowTaskViewButton)"
[ "$(regq 'HKCU\Software\Stained Glass\Style' Frame)" = 0x0 ] && [ "$(regq "$WM" CaptionHeight)" = 18 ] && [ "$(regq "$WM" BorderWidth)" = 1 ] \
    && pass "and the flat frames at their old sizes" || fail "Classic frames: frame $(regq 'HKCU\Software\Stained Glass\Style' Frame) caption $(regq "$WM" CaptionHeight) border $(regq "$WM" BorderWidth)"
wine "$T/sg-settings64.exe" --set style glass 2>/dev/null | tr -d '\r' | grep -q '^OK' && [ "$(regq 'HKCU\Software\Stained Glass\Style' Frame)" = 0x2 ] && [ "$(TBS)" = 0x0 ] \
    && wine "$T/sg-settings64.exe" --dump personalization 2>/dev/null | tr -d '\r' | grep -qx 'style=glass' \
    && pass "--set style glass: Glass frames with the flat taskbar (the looks mix)" || fail "style glass: frame $(regq 'HKCU\Software\Stained Glass\Style' Frame) bar $(TBS)"
wine "$T/sg-settings64.exe" --set style classic >/dev/null 2>&1

# --- every part its own look: frames, taskbar, Start; Themes says Custom (David 2026-10-02) --------
sgs() { wine "$T/sg-settings64.exe" --set "$@" 2>/dev/null | tr -d '\r'; }
pdump() { wine "$T/sg-settings64.exe" --dump personalization 2>/dev/null | tr -d '\r'; }
SGK='HKCU\Software\Stained Glass'
sgs look rounded >/dev/null
[ "$(sgs start-look horizon)" = OK ] && [ "$(regq "$SGK\Start" Look)" = 0x2 ] && [ "$(regq "$SGK\Taskbar" Look)" = 0x1 ] \
    && [ "$(regq "$SGK\Style" Rounded)" = 0x1 ] && [ "$(regq "$ADVK" TaskbarAl)" = 0x1 ] && pdump | grep -qx 'look=custom' \
    && pdump | grep -qx 'start.look=horizon' \
    && pass "Rounded, then Start's own look Horizon: the centred Rounded taskbar and round windows stay, the look is custom" \
    || fail "start-look: start $(regq "$SGK\Start" Look) bar $(regq "$SGK\Taskbar" Look) rounded $(regq "$SGK\Style" Rounded) $(pdump | grep '^look=')"
wine start ms-settings:themes >/dev/null 2>&1
page_is Themes "ms-settings:themes (custom)"
sleep 0.5
tr -d '\r' < "$DUMP" | grep -q '^control ComboBox .*: Custom$' && pass "Themes shows Custom" || fail "Themes custom: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"
wine start ms-settings:personalization-start >/dev/null 2>&1
page_is Start "ms-settings:personalization-start (style)"
sleep 0.5
has ": Horizon: two columns and All Programs" && pass "Start shows its own style: Horizon" || fail "Start style: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"
[ "$(sgs taskbar-look glass)" = OK ] && [ "$(regq "$SGK\Taskbar" Look)" = 0x3 ] && [ "$(TBS)" = 0x2 ] && pdump | grep -qx 'taskbar.look=glass' \
    && pass "the taskbar's own look: Glass (Taskbar Look 3, Style 2)" || fail "taskbar-look glass: $(regq "$SGK\Taskbar" Look) $(TBS)"
wine start ms-settings:taskbar >/dev/null 2>&1
page_is Taskbar "ms-settings:taskbar (look)"
sleep 0.5
has ": Glass: dark glass" && pass "Taskbar shows its own style: Glass" || fail "Taskbar style: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"
sgs look glass >/dev/null
pdump | grep -qx 'look=glass' && [ "$(regq "$SGK\Start" Look)" = 0x3 ] && [ "$(regq "$SGK\Taskbar" Look)" = 0x3 ] && [ "$(regq "$SGK\Style" Frame)" = 0x2 ] \
    && pass "a look chosen again sets every part to it: all Glass" || fail "look glass: $(pdump | grep 'look=')"
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/stained-glass/effects.conf"
grep -qx 'frame=glass' "$CONF" && grep -qx 'glass=62' "$CONF" && grep -qx "caption=$(( 21 + 1 ))" "$CONF" \
    && pass "the compositor learns the frames' look and sizes, and Glass's see-through (frame=glass caption=22 glass=62)" \
    || fail "effects.conf: $(tr '\n' ' ' < "$CONF")"
sgs effects transparency off >/dev/null
grep -qx 'glass=0' "$CONF" && pass "Transparency effects off: the Glass frames solid (glass=0)" || fail "transparency off: $(grep glass= "$CONF")"
sgs effects transparency on >/dev/null
# the title bars take a share of the screen: at 1080 px the Glass caption 21 -> 29, its frame 7 -> 10
# (as the session asks at sign-in and when the screen changes: --set metrics;
# the X server here is 768 px tall, so its height is said)
SG_FAKE_SCREEN_HEIGHT=1080 sgs metrics >/dev/null
[ "$(regq "$WM" CaptionHeight)" = 29 ] && [ "$(regq "$WM" BorderWidth)" = 10 ] && [ "$(regq "$SGK\\Style" Scale8)" = 0xb ] \
    && pass "at 1080 px the title bars are 11/8 of their size: caption 29, frame 10" \
    || fail "1080p: caption $(regq "$WM" CaptionHeight) border $(regq "$WM" BorderWidth) scale $(regq "$SGK\\Style" Scale8)"
SG_FAKE_SCREEN_HEIGHT=1080 sgs title-scale off >/dev/null
[ "$(regq "$WM" CaptionHeight)" = 21 ] && pass "Size title bars to the screen off: their own size (21)" || fail "title-scale off: $(regq "$WM" CaptionHeight)"
sgs title-scale on >/dev/null
[ "$(regq "$WM" CaptionHeight)" = 21 ] && [ "$(regq "$WM" BorderWidth)" = 7 ] && pass "back at 1024x768: their own size again" \
    || fail "back at 1024x768: caption $(regq "$WM" CaptionHeight) border $(regq "$WM" BorderWidth)"
[ "$(sgs background-animation cells)" = OK ] && [ "$(regq "$SGK\Effects" AnimatedBackground)" = 0x2 ] && grep -qx 'background=cells' "$CONF" \
    && pass "the animated background: Living glass (background=cells for the compositor)" || fail "background animation: $(regq "$SGK\Effects" AnimatedBackground) $(grep background= "$CONF")"
wine start ms-settings:personalization-background >/dev/null 2>&1
page_is Background "ms-settings:personalization-background (animated)"
sleep 0.5
has ": Living glass: the picture as stained glass round your windows" && pass "Background shows it, in its one list" || fail "Background page: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox')"
sgs background-animation static >/dev/null
sgs look classic >/dev/null

# --- Share & reset: a look saved, reset, read back; a shared file sets only look choices ---------
SG='C:\users\Public'
DWMK='HKCU\Software\Microsoft\Windows\DWM'
TBK='HKCU\Software\Stained Glass\Taskbar'
sgset() { wine "$T/sg-settings64.exe" --set "$@" 2>/dev/null | tr -d '\r' | tail -1; }
sgset look rounded >/dev/null; sgset accent 112233 >/dev/null; sgset mode apps dark >/dev/null
wine reg add "$TBK" /v Position /t REG_DWORD /d 1 /f >/dev/null 2>&1
[ "$(sgset look export "$SG\\mine.sglook")" = OK ] && grep -q '^Style=rounded' "$WINEPREFIX/drive_c/users/Public/mine.sglook" \
    && grep -q '^Accent=112233' "$WINEPREFIX/drive_c/users/Public/mine.sglook" && grep -q '^Position=1' "$WINEPREFIX/drive_c/users/Public/mine.sglook" \
    && pass "Share & reset: the look is saved to a .sglook file (style, accent, taskbar)" || fail "look export: $(tr -d '\r' < "$WINEPREFIX/drive_c/users/Public/mine.sglook" 2>&1 | head -8 | tr '\n' ' ')"
sgset look reset >/dev/null
[ "$(regq 'HKCU\Software\Stained Glass\Style' Rounded)" = 0x0 ] && [ "$(regq "$DWMK" AccentColor)" = 0xffbe2f7b ] \
    && [ "$(regq 'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' AppsUseLightTheme)" = 0x1 ] && [ -z "$(regq "$TBK" Position)" ] \
    && pass "reset puts back the default look: Classic, the Stained Glass accent, light, the taskbar's own place" \
    || fail "look reset: rounded $(regq 'HKCU\Software\Stained Glass\Style' Rounded) accent $(regq "$DWMK" AccentColor) position $(regq "$TBK" Position)"
[ "$(sgset look import "$SG\\mine.sglook")" = OK ] && [ "$(regq 'HKCU\Software\Stained Glass\Style' Rounded)" = 0x1 ] \
    && [ "$(regq "$DWMK" AccentColor)" = 0xff332211 ] && [ "$(regq "$TBK" Position)" = 0x1 ] \
    && pass "a saved look is read back (Rounded, its accent, the taskbar at the top)" \
    || fail "look import: rounded $(regq 'HKCU\Software\Stained Glass\Style' Rounded) accent $(regq "$DWMK" AccentColor) position $(regq "$TBK" Position)"
printf '[Look]\r\nFormat=1\r\n[Taskbar]\r\nPosition=99\r\n[HKEY_CURRENT_USER\\Software\\SgLookEvil]\r\n"x"=dword:1\r\n' > "$WINEPREFIX/drive_c/users/Public/evil.sglook"
sgset look import "$SG\\evil.sglook" >/dev/null
[ "$(regq "$TBK" Position)" = 0x1 ] && ! wine reg query 'HKCU\Software\SgLookEvil' >/dev/null 2>&1 \
    && pass "a shared file sets only look choices, within their range (Position=99 and a registry section ignored)" \
    || fail "look import took more than a look: position $(regq "$TBK" Position)"
printf '[Other]\r\nx=1\r\n' > "$WINEPREFIX/drive_c/users/Public/not.sglook"
case "$(sgset look import "$SG\\not.sglook")" in FAILED*) pass "a file that is not a saved look is refused" ;; *) fail "a non-look file was taken" ;; esac
sgset look classic >/dev/null
wine start ms-settings:personalization-share >/dev/null 2>&1
page_is "Share & reset" "ms-settings:personalization-share"
sleep 0.5
has "Save my look..." && has "Use a saved look..." && has "Reset to the default look" \
    && pass "Personalization > Share & reset: save, use and reset" || fail "Share & reset page: $(tr -d '\r' < "$DUMP" | grep -c '^control')"

# --- Effects: Show animations (Windows' own switch) and the desktop slide (David 2026-09-29) --------
EFK='HKCU\Software\Stained Glass\Effects'
upm() { wine reg query 'HKCU\Control Panel\Desktop' /v UserPreferencesMask 2>/dev/null | tr -d '\r' | awk '/UserPreferencesMask/{print substr($3,9,2)}'; }
[ "$(sgset effects slide off)" = OK ] && [ "$(regq "$EFK" SlideDesktops)" = 0x0 ] && [ "$(sgset effects slide on)" = OK ] && [ "$(regq "$EFK" SlideDesktops)" = 0x1 ] \
    && pass "Effects: sliding between desktops is kept (Effects\\SlideDesktops)" || fail "effects slide: $(regq "$EFK" SlideDesktops)"
sgset effects animations off >/dev/null
b=$(upm)
wine start ms-settings:personalization-effects >/dev/null 2>&1
page_is Effects "ms-settings:personalization-effects"
sleep 0.5
anim=$(tr -d '\r' < "$DUMP" | grep '^control SgSetCtl ' | grep -F ': Show animations' | sed -n 's/.* state=\([0-9]*\).*/\1/p')
[ -n "$b" ] && [ $((0x$b & 2)) -eq 0 ] && [ "$anim" = 0 ] && has "Slide between virtual desktops" \
    && pass "Show animations off is Windows' own setting (UserPreferencesMask), and the page shows it off" \
    || fail "effects animations: mask byte $b, toggle $anim"
sgset effects animations on >/dev/null
b=$(upm); [ -n "$b" ] && [ $((0x$b & 2)) -ne 0 ] && pass "and back on" || fail "animations did not come back on (mask byte $b)"
# Colors > Transparency effects: the taskbar frosted (wine-sg 0745), told at once
PK='HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
TBW() { xwininfo -root -tree 2>/dev/null | awk '/"shell - Wine Desktop"/ { d = 1 } d && /x40\+0\+|x48\+0\+/ { print $1; exit }'; }
[ "$(sgset effects transparency off)" = OK ] && [ "$(regq "$PK" EnableTransparency)" = 0x0 ] && sleep 1.5 \
    && xprop -id "$(TBW)" _SG_ACRYLIC 2>&1 | grep -q 'not found' \
    && [ "$(sgset effects transparency on)" = OK ] && sleep 1.5 && xprop -id "$(TBW)" _SG_ACRYLIC 2>&1 | grep -q '= 85' \
    && pass "Transparency effects off and on reach the taskbar at once (_SG_ACRYLIC)" \
    || fail "transparency: $(regq "$PK" EnableTransparency) taskbar $(TBW): $(xprop -id "$(TBW)" _SG_ACRYLIC 2>&1)"
# the window effects, drawn by the desktop's compositor (sg-compositor's
# sg-deskcomp): kept here, and written where it reads them
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/stained-glass/effects.conf"
cv() { sed -n "s/^$1=//p" "$CONF" 2>/dev/null; }
[ "$(sgset effects wobbly on)" = OK ] && [ "$(sgset effects open zoom)" = OK ] && [ "$(sgset effects minimize lamp)" = OK ] \
    && [ "$(sgset effects shadows off)" = OK ] && [ "$(sgset effects moving on)" = OK ] \
    && [ "$(regq "$EFK" Wobbly)" = 0x1 ] && [ "$(regq "$EFK" WindowOpen)" = 0x2 ] && [ "$(regq "$EFK" WindowMinimize)" = 0x2 ] \
    && [ "$(cv wobbly)" = 1 ] && [ "$(cv open)" = zoom ] && [ "$(cv minimize)" = lamp ] && [ "$(cv shadows)" = 0 ] \
    && [ "$(cv moving)" = 1 ] && [ "$(cv animations)" = 1 ] \
    && pass "the window effects are kept and written for the compositor ($(grep -v '^#' "$CONF" | tr '\n' ' '))" \
    || fail "window effects: wobbly $(regq "$EFK" Wobbly) open $(regq "$EFK" WindowOpen); conf: $(cat "$CONF" 2>&1 | tr '\n' ' ')"
sgset effects animations off >/dev/null
[ "$(cv animations)" = 0 ] && pass "Show animations off reaches the compositor too" || fail "conf animations: $(cv animations)"
# a session has no XDG_CONFIG_HOME: the file goes to ~/.config (Wine hands $HOME over as WINEHOMEDIR)
rm -f "$HOME/.config/stained-glass/effects.conf"
env -u XDG_CONFIG_HOME wine "$T/sg-settings64.exe" --set effects wobbly off >/dev/null 2>&1
grep -qx 'wobbly=0' "$HOME/.config/stained-glass/effects.conf" 2>/dev/null \
    && pass "without XDG_CONFIG_HOME the effects go to ~/.config/stained-glass/effects.conf" \
    || fail "no ~/.config effects.conf: $(ls "$HOME/.config/stained-glass" 2>&1)"
sgset effects wobbly on >/dev/null
sgset effects animations on >/dev/null
wine start ms-settings:personalization-effects >/dev/null 2>&1
page_is Effects "ms-settings:personalization-effects (windows)"
sleep 0.5
has ": Zoom" && has ": Magic lamp" && has "Wobbly windows while dragging" && has "Shadows under windows and menus" \
    && pass "the Effects page shows the window effects" || fail "Effects page: $(tr -d '\r' < "$DUMP" | grep '^control' | grep -i 'combo\|wobbl\|shadow' | head -6)"
sgset effects shadows on >/dev/null; sgset effects wobbly off >/dev/null; sgset effects open none >/dev/null
sgset effects minimize none >/dev/null; sgset effects moving off >/dev/null

# --- Start style: Classic tiles is the default, the Rounded option is not 'Recommended' (David 2026-09-29) ---
# the combo dump shows only the selected item, so check each selection in turn
wine reg delete 'HKCU\Software\Stained Glass\Start' /v Look /f >/dev/null 2>&1
wine reg add 'HKCU\Software\Stained Glass\Start' /v Centered /t REG_DWORD /d 0 /f >/dev/null 2>&1
wine start ms-settings:personalization-start >/dev/null 2>&1
page_is Start "ms-settings:personalization-start (Tiles default)"
sleep 0.5
tr -d '\r' < "$DUMP" | grep -q '^control ComboBox id=2007 .*: Classic: tiles (the default)' \
    && pass "Start style defaults to Classic tiles" || fail "Start default: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox id=2007')"
wine reg add 'HKCU\Software\Stained Glass\Start' /v Centered /t REG_DWORD /d 1 /f >/dev/null 2>&1
wine start ms-settings:personalization-start >/dev/null 2>&1
page_is Start "ms-settings:personalization-start (centred label)"
sleep 0.5
has "Rounded: pinned and recent apps, centered" && ! has "Pinned and Recommended" \
    && pass "Centred option is labelled without 'Recommended'" \
    || fail "Centred label: $(tr -d '\r' < "$DUMP" | grep '^control ComboBox id=2007')"
wine reg add 'HKCU\Software\Stained Glass\Start' /v Centered /t REG_DWORD /d 0 /f >/dev/null 2>&1

if [ "${WINI:-0}" = 1 ]; then
    wine start ms-settings:about >/dev/null 2>&1; page_is About "(before Win+I)"
    xdotool key super+i; sleep 2
    page_is Home "Win+I"
fi

[ $RC = 0 ] && echo "settings-check: PASS" || echo "settings-check: FAIL"
exit $RC
