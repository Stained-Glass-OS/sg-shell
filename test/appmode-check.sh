#!/bin/sh
# Gate: our own apps follow the app mode (Settings > Personalization > Colors,
# AppsUseLightTheme), live. Each app is opened in light mode on a shell
# desktop, then the mode goes dark exactly as Settings does it
# (AppsUseLightTheme=0 + WM_SETTINGCHANGE "ImmersiveColorSet") and back.
# The app's own drawing (the mean brightness of its client area, or of the
# part that is its chrome) and its title bar (DWMWA_USE_IMMERSIVE_DARK_MODE)
# must go dark and come back light, without restarting it.
#
# Needs a wine-sg with dark title bars (0162); SG_WINE / SG_WINESERVER name
# another build. Screenshots: build/appmode-<app>-{light,dark,back}.png.
# SG_APPMODE_ONLY="calc paint" runs only those apps.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE="${SG_WINE:-/opt/wine-sg/bin/wine}"
export WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
OUT="$HERE/build"
RC=0; DPY=${SG_DISPLAY:-113}; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb import convert x86_64-w64-mingw32-gcc python3; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE" ] && [ -f "$HERE/build/sg-calc64.exe" ] || { echo "SKIP: wine-sg or the build missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-appmode.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINESERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

x86_64-w64-mingw32-gcc -O2 -municode -o "$T/probe.exe" "$HERE/test/appmode-probe.c" || { fail "probe did not build"; exit 1; }
# a display nobody else has (other gates run beside this one)
while [ -e "/tmp/.X${DPY}-lock" ] || [ -e "/tmp/.X11-unix/X${DPY}" ]; do DPY=$((DPY + 1)); done
Xvfb ":$DPY" -screen 0 1280x800x24 -ac -nolisten tcp >/dev/null 2>&1 & XP=$!
sleep 1
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
"$WINE" wineboot --init >/dev/null 2>&1; "$WINESERVER" -w
reg() { "$WINE" reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800
for f in "$HERE"/theme/*.reg; do "$WINE" reg import "$("$WINE" winepath -w "$f" | tr -d '\r')" >/dev/null 2>&1; done
reg 'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' /v AppsUseLightTheme /t REG_DWORD /d 1
"$WINESERVER" -w
python3 -c "import zipfile; z = zipfile.ZipFile('$T/a.zip', 'w'); z.writestr('readme.txt', 'hello'); z.close()"
ZIPW=$("$WINE" winepath -w "$T/a.zip" | tr -d '\r')

"$WINE" explorer /desktop=shell,1280x800 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
sleep 2

P() { "$WINE" "$T/probe.exe" "$@" 2>/dev/null | tr -d '\r'; }
# the mean brightness (0-100) of a screen rectangle in a screenshot
mean() { convert "$1" -crop "$(($4 - $2))x$(($5 - $3))+$2+$3" +repage -colorspace Gray -format '%[fx:int(100*mean)]' info: 2>/dev/null; }

# app  class  exe  args  chrome  (chrome: which part of the client is the app's own drawing --
# all, top (the top 15%: a ribbon over a white canvas), bottom (the bottom 20%: a transport bar))
APPS='calc SgCalculator sg-calc64.exe - all
taskmgr SgTaskManagerWindow sg-taskmgr64.exe - all
photos SgPhotosWindow sg-photos64.exe - all
media SgMediaPlayer sg-media64.exe - bottom
clock SgClockWindow sg-clock64.exe - all
charmap SgCharMap sg-charmap64.exe - all
paint SgPaintMain sg-paint64.exe - top
snip SgSnippingTool sg-snip64.exe - all
zip SgZipBrowse sg-zip64.exe ZIP all
mmc MMCMainFrame sg-mmc64.exe services.msc all'

check() {   # app class stage: prints "<title mean> <client mean>"
    stage=$3
    set -- $(P rect "$2")
    [ "$1" = none ] || [ -z "${1:-}" ] && { echo "- -"; return; }
    wl=$1; wt=$2; wr=$3; ox=$5; oy=$6; cr=$7; cb=$8
    shot="$T/s.png"; import -window root "$shot" 2>/dev/null
    tl=$(( wl + (wr - wl) * 35 / 100 )); tr=$(( wl + (wr - wl) * 55 / 100 ))
    title=$(mean "$shot" "$tl" $(( wt + 6 )) "$tr" $(( oy - 6 )))
    case "$CHROME" in
    top) cb2=$(( oy + (cb - oy) * 15 / 100 )); client=$(mean "$shot" "$ox" "$oy" "$cr" "$cb2") ;;
    bottom) ct2=$(( cb - (cb - oy) * 20 / 100 )); client=$(mean "$shot" "$ox" "$ct2" "$cr" "$cb") ;;
    *) client=$(mean "$shot" "$ox" "$oy" "$cr" "$cb") ;;
    esac
    cp "$shot" "$OUT/appmode-$APP-$stage.png"
    echo "$title $client"
}

echo "$APPS" | while read -r APP CLASS EXE ARGS CHROME; do
    [ -n "${SG_APPMODE_ONLY:-}" ] && ! echo " $SG_APPMODE_ONLY " | grep -q " $APP " && continue
    [ -f "$HERE/build/$EXE" ] || { fail "$APP: $EXE not built"; continue; }
    cp "$HERE/build/$EXE" "$T/"
    case "$ARGS" in -) a="" ;; ZIP) a="$ZIPW" ;; *) a="$ARGS" ;; esac
    "$WINE" "$T/$EXE" $a >/dev/null 2>&1 &
    i=0; while [ "$(P rect "$CLASS")" = none ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
    sleep 3
    set -- $(check "$APP" "$CLASS" light); lt=$1; lc=$2
    P mode dark; sleep 4
    set -- $(check "$APP" "$CLASS" dark); dt=$1; dc=$2
    P mode light; sleep 4
    set -- $(check "$APP" "$CLASS" back); bt=$1; bc=$2
    echo "      $APP: title $lt -> $dt -> $bt, own drawing $lc -> $dc -> $bc"
    if [ "$lt" = - ]; then fail "$APP: no $CLASS window"
    else
        [ "$lc" -ge 55 ] && [ "$lt" -ge 80 ] && pass "$APP starts light" || fail "$APP light: title $lt, drawing $lc"
        [ "$dc" -le 35 ] && pass "$APP's own drawing goes dark with the app mode" || fail "$APP dark: drawing $dc"
        [ "$dt" -le 25 ] && pass "$APP's title bar goes dark" || fail "$APP dark: title $dt"
        [ "$bc" -ge 55 ] && [ "$bt" -ge 80 ] && pass "$APP comes back light" || fail "$APP back: title $bt, drawing $bc"
    fi
    P close "$CLASS"; sleep 2
    echo "$RC" > "$T/rc"
done
[ -f "$T/rc" ] && RC=$(cat "$T/rc")
echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
