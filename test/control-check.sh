#!/bin/sh
# Gate for the Control Panel (sg-control). Each applet's --dump is checked
# against the machine it runs on -- the account database, the clock's zone,
# apt's history, the network interfaces -- and what the applets change is read
# back from where Windows keeps it. The elevated half runs end to end through
# a real spool with sg-admind (in its unprivileged test mode, with stand-ins
# for the system tools), including one dialog driven by the keyboard. Then the
# window: it appears, paints, and the keyboard navigates it.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
CTL="$HERE/build/sg-control64.exe"
ADMIND="$HERE/admin/sg-admind"
RC=0; DPY=90; T=$(mktemp -d); chmod 755 "$T"; XP=""; LOOP=""
export HOME="$T"
# shellcheck disable=SC2317
cleanup() {
    [ -n "$LOOP" ] && kill "$LOOP" 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

[ -x "$WINE_DIR/bin/wine" ] && [ -f "$CTL" ] || { echo "SKIP: wine-sg or sg-control not built"; exit 77; }
export WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
ctl() { wine "$CTL" "$@" 2>/dev/null </dev/null | tr -d '\r'; }
val() { printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -1; }
regq() { wine reg query "$1" /v "$2" 2>/dev/null | tr -d '\r' | awk -v v="$2" '$1 == v { $1 = ""; $2 = ""; sub(/^ +/, ""); print }'; }

# ---- 1. System ------------------------------------------------------------------
out=$(ctl --dump system)
[ "$(val "$out" edition)" = "Stained Glass OS" ] && pass "System: the edition" || fail "edition: $(val "$out" edition)"
comp=$(val "$out" computer)
[ -n "$comp" ] && [ "$comp" = "$(hostname | cut -c1-15 | tr '[:lower:]' '[:upper:]')" ] \
    && pass "System: the computer name is this machine's ($comp)" || fail "computer name: $comp vs $(hostname)"
[ "$(val "$out" membership)" = WORKGROUP ] || [ -f /etc/stained-glass/role ] \
    && pass "System: a machine with no role is in a workgroup" || fail "membership: $(val "$out" membership)"
usr=$(val "$out" user)
[ "${usr##*\\}" = "$(id -un)" ] && pass "System: the signed-in user ($usr)" || fail "user: $usr"
for k in arch osbuild cpu; do [ -n "$(val "$out" $k)" ] || fail "no $k"; done
val "$out" ram | grep -qE '^[0-9]+\.[0-9] GB$' && pass "System: installed RAM ($(val "$out" ram))" || fail "RAM: $(val "$out" ram)"
val "$out" policies | grep -qE '^[0-9]+$' && pass "System: the policy count" || fail "no policy count"
want=standard; id -nG | tr ' ' '\n' | grep -qx sg-admins && want=administrator
[ "$(val "$out" accounttype)" = "$want" ] && pass "System: account type from sg-admins membership ($want)" || fail "account type: $(val "$out" accounttype)"

# ---- 2. Programs and Features -----------------------------------------------------
U='HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall'
wine reg add "$U\SgTestApp" /v DisplayName /d "SG Test App" /f >/dev/null 2>&1
wine reg add "$U\SgTestApp" /v Publisher /d "Stained Glass Test" /f >/dev/null 2>&1
wine reg add "$U\SgTestApp" /v DisplayVersion /d "1.2.3" /f >/dev/null 2>&1
wine reg add "$U\SgTestApp" /v EstimatedSize /t REG_DWORD /d 2048 /f >/dev/null 2>&1
wine reg add "$U\SgTestApp" /v InstallDate /d 20260924 /f >/dev/null 2>&1
wine reg add "$U\SgTestApp" /v UninstallString /d 'cmd.exe /c echo removed> C:\sg-uninstalled.txt' /f >/dev/null 2>&1
wine reg add "$U\SgHidden" /v DisplayName /d "SG Hidden Component" /f >/dev/null 2>&1
wine reg add "$U\SgHidden" /v SystemComponent /t REG_DWORD /d 1 /f >/dev/null 2>&1
wine reg add "$U\SgPatch" /v DisplayName /d "SG Test App Update" /f >/dev/null 2>&1
wine reg add "$U\SgPatch" /v ParentKeyName /d "SgTestApp" /f >/dev/null 2>&1
G='{12345678-1234-1234-1234-123456789ABC}'
wine reg add "$U\\$G" /v DisplayName /d "SG MSI App" /f >/dev/null 2>&1
wine reg add "$U\\$G" /v WindowsInstaller /t REG_DWORD /d 1 /f >/dev/null 2>&1
out=$(ctl --dump programs)
echo "$out" | grep -qx 'program=SG Test App|Stained Glass Test|1.2.3|.*|2048|user|cmd.exe /c echo removed> C:\\sg-uninstalled.txt' \
    && pass "Programs: an installed program with its publisher, version, size and uninstaller" \
    || fail "program line: $(echo "$out" | grep 'SG Test App')"
echo "$out" | grep -q 'SG Hidden Component' && fail "listed a system component" || pass "Programs: system components are hidden"
echo "$out" | grep -q 'SG Test App Update' && fail "listed an update as a program" || pass "Programs: updates are not listed as programs"
echo "$out" | grep -q "^program=SG MSI App|.*|msiexec.exe /x$G\$" && pass "Programs: a Windows Installer product uninstalls through msiexec /x" \
    || fail "msi: $(echo "$out" | grep 'SG MSI App')"
[ "$(val "$out" programs.count)" = "$(echo "$out" | grep -c '^program=')" ] && pass "Programs: the count matches the list" || fail "count"
ctl --uninstall "SG Test App" >/dev/null
[ -f "$T/pfx/drive_c/sg-uninstalled.txt" ] && pass "Programs: Uninstall runs the program's own uninstaller" || fail "the uninstaller did not run"

# ---- 3. User Accounts -----------------------------------------------------------------
out=$(ctl --dump users)
[ "$(val "$out" user.current)" = "$(id -un)" ] && pass "Users: the current account" || fail "current: $(val "$out" user.current)"
want=$(getent passwd | awk -F: '$3 >= 1000 && $3 < 60000 && $7 !~ /(nologin|false)$/ { print $1 }' | sort | tr '\n' ' ')
got=$(echo "$out" | sed -n 's/^account=\([^|]*\)|.*/\1/p' | sort | tr '\n' ' ')
[ "$got" = "$want" ] && pass "Users: the local accounts are the machine's (${got% })" || fail "accounts: [$got] vs [$want]"
bad=0
for a in $got; do
    t=$(echo "$out" | sed -n "s/^account=$a|[^|]*|//p")
    w=standard; getent group sg-admins | cut -d: -f4 | tr ',' '\n' | grep -qx "$a" && w=administrator
    [ "$t" = "$w" ] || bad=1
done
[ $bad = 0 ] && pass "Users: each account's type follows sg-admins" || fail "account types"

# ---- 4. Date and Time --------------------------------------------------------------------
out=$(ctl --dump datetime)
zone=$(readlink /etc/localtime | sed 's|.*/zoneinfo/||')
[ -n "$zone" ] && [ "$(val "$out" timezone.iana)" = "$zone" ] && pass "Date and Time: the zone is the clock's ($zone)" \
    || fail "zone: $(val "$out" timezone.iana) vs $zone"
[ -n "$(val "$out" timezone.display)" ] && pass "Date and Time: the Windows zone name ($(val "$out" timezone.windows))" || fail "no Windows zone"
want=off; [ -e /etc/systemd/system/sysinit.target.wants/systemd-timesyncd.service ] && want=on
[ "$(val "$out" ntp)" = "$want" ] && pass "Date and Time: Internet time is $want, as systemd has it" || fail "ntp: $(val "$out" ntp)"

# ---- 5. Personalization -------------------------------------------------------------------
if command -v convert >/dev/null; then
    mkdir -p "$T/pfx/drive_c/windows/Web/Wallpaper/Test"
    convert -size 1600x900 xc:'#2040C0' -fill '#F0A020' -draw 'rectangle 700,350 900,550' "$T/pfx/drive_c/windows/Web/Wallpaper/Test/pic.png"
    r=$(ctl --set wallpaper 'C:\windows\Web\Wallpaper\Test\pic.png' fill)
    out=$(ctl --dump personalization)
    wp=$(regq 'HKCU\Control Panel\Desktop' Wallpaper)
    bmp="$T/pfx/drive_c/users/$(id -un)/AppData/Roaming/Microsoft/Windows/Themes/TranscodedWallpaper"
    [ "$r" = OK ] && [ "$wp" = "C:\\users\\$(id -un)\\AppData\\Roaming\\Microsoft\\Windows\\Themes\\TranscodedWallpaper" ] \
        && pass "Personalization: a picture becomes the desktop through SPI_SETDESKWALLPAPER" || fail "wallpaper: $r / $wp"
    [ "$(regq 'HKCU\Control Panel\Desktop' WallpaperStyle)" = 10 ] && [ "$(regq 'HKCU\Control Panel\Desktop' TileWallpaper)" = 0 ] \
        && pass "Personalization: 'Fill' is WallpaperStyle 10, TileWallpaper 0" || fail "style values"
    [ "$(val "$out" wallpaper.source)" = 'C:\windows\Web\Wallpaper\Test\pic.png' ] && \
        [ "$(regq 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Wallpapers' BackgroundHistoryPath0)" = 'C:\windows\Web\Wallpaper\Test\pic.png' ] \
        && pass "Personalization: the original picture is kept in the wallpaper history" || fail "source: $(val "$out" wallpaper.source)"
    if [ -f "$bmp" ] && info=$(convert "$bmp" -format '%m %w %h %[pixel:p{%[fx:w/2],%[fx:h/2]}] %[pixel:p{2,2}]' info: 2>/dev/null); then
        set -- $info
        case $1 in BMP*) fmt=BMP ;; *) fmt=$1 ;; esac
        [ "$fmt" = BMP ] && [ "$4" = "srgb(240,160,32)" ] && [ "$5" = "srgb(32,64,192)" ] \
            && pass "Personalization: the desktop's copy is a ${2}x${3} BMP of the picture" || fail "transcoded: $info"
    else fail "no transcoded wallpaper"; fi
    ctl --set wallpaper 'C:\windows\Web\Wallpaper\Test\pic.png' tile >/dev/null
    [ "$(regq 'HKCU\Control Panel\Desktop' TileWallpaper)" = 1 ] && [ "$(convert "$bmp" -format '%w' info:)" = 1600 ] \
        && pass "Personalization: 'Tile' keeps the picture's own size and sets TileWallpaper" || fail "tile"
    r=$(ctl --set background 1E552E)
    [ "$r" = OK ] && [ "$(regq 'HKCU\Control Panel\Colors' Background)" = "30 85 46" ] && [ -z "$(regq 'HKCU\Control Panel\Desktop' Wallpaper)" ] \
        && pass "Personalization: a solid color clears the picture and sets Colors\\Background" || fail "background: $r"
else echo "info  no ImageMagick; skipped the wallpaper checks"; fi
r=$(ctl --set accent 107C41)
[ "$r" = OK ] && [ "$(regq 'HKCU\Software\Microsoft\Windows\DWM' AccentColor)" = 0xff417c10 ] \
    && [ "$(regq 'HKCU\Software\Microsoft\Windows\DWM' ColorizationColor)" = 0xc4107c41 ] \
    && [ "$(regq 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Accent' AccentColorMenu)" = 0xff417c10 ] \
    && pass "Personalization: the accent color is DWM's AccentColor (ABGR) and ColorizationColor (ARGB)" || fail "accent: $r"
regq 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Accent' AccentPalette | grep -qi '^107c41ff' 2>/dev/null \
    || wine reg query 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Accent' /v AccentPalette 2>/dev/null | grep -qi '107c41ff' \
    && pass "Personalization: ... with an AccentPalette around it" || fail "no AccentPalette"
ctl --set mode apps dark >/dev/null; ctl --set mode system light >/dev/null
[ "$(regq 'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' AppsUseLightTheme)" = 0x0 ] && \
[ "$(regq 'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' SystemUsesLightTheme)" = 0x1 ] \
    && pass "Personalization: app and Windows modes are AppsUseLightTheme / SystemUsesLightTheme" || fail "modes"
out=$(ctl --dump personalization)
[ "$(val "$out" accent)" = 107C41 ] && [ "$(val "$out" mode.apps)" = dark ] && [ "$(val "$out" background.type)" = solid ] \
    && pass "Personalization: the page reads back what was set" || fail "readback: $out"
r=$(ctl --set accent nothex); [ "${r%% *}" = FAILED ] && pass "Personalization: a malformed color is refused" || fail "accepted 'nothex'"

# ---- 6. Windows Update ------------------------------------------------------------------------
out=$(ctl --dump update)
want=no; [ -e /system-update ] && want=yes
[ "$(val "$out" update.pending)" = $want ] && pass "Update: restart pending = $want, from /system-update" || fail "pending"
if [ -e /var/lib/systemd/timers/stamp-sg-update-prepare.timer ]; then
    [ "$(val "$out" update.lastcheck)" != never ] && pass "Update: the last check time" || fail "last check"
else [ "$(val "$out" update.lastcheck)" = never ] && pass "Update: never checked (no timer stamp here)" || fail "last check"; fi
if [ -r /var/log/apt/history.log ]; then
    last=$(sed -n 's/^Start-Date: //p' /var/log/apt/history.log | tail -1)
    [ "$(echo "$out" | sed -n 's/^update.history=\([^|]*\)|.*/\1/p' | head -1)" = "$last" ] \
        && pass "Update: the history is apt's, newest first ($last)" || fail "history: $(echo "$out" | grep history | head -1)"
fi
n=$(cat /etc/apt/sources.list.d/*.sources 2>/dev/null | grep -c '^URIs:')
[ "$(echo "$out" | grep -c '^update.source=')" = "$n" ] && pass "Update: the update sources ($n)" || fail "sources"

# ---- 7. Network and Sharing Center -------------------------------------------------------------
out=$(ctl --dump network)
ip4=$(ip -4 -o addr show scope global 2>/dev/null | awk '{ print $4; exit }')
if [ -n "$ip4" ]; then
    echo "$out" | grep -q "^adapter=.*|up|.*$ip4" && pass "Network: the active adapter and its address ($ip4)" || fail "no $ip4 in: $out"
fi
gw=$(ip -4 route show default 2>/dev/null | awk '{ print $3; exit }')
[ -z "$gw" ] || echo "$out" | grep -q "^adapter=.*|up|[^|]*|$gw|" && pass "Network: the default gateway${gw:+ ($gw)}" || fail "gateway $gw"

# ---- 8. control.exe's arguments -----------------------------------------------------------------
res() { ctl --resolve "$1"; }
ok=1
for pair in 'appwiz.cpl=page=Programs and Features' 'timedate.cpl=page=Date and Time' 'sysdm.cpl=page=System' \
            'C:\Windows\System32\appwiz.cpl,,2=page=Programs and Features' 'userpasswords=page=User Accounts' \
            'userpasswords2=page=Manage Accounts' 'desktop=page=Personalization' 'Microsoft.WindowsUpdate=page=Updates' \
            'Microsoft.NetworkAndSharingCenter=page=Network and Sharing Center' 'inetcpl.cpl=cpl=inetcpl.cpl' \
            'desk.cpl,@0=cpl=desk.cpl' 'Microsoft.GameControllers=cpl=joy.cpl' 'thirdparty.cpl=cpl=thirdparty.cpl' \
            'nonsense=page=Control Panel'; do
    a=${pair%%=*}; w=${pair#*=}
    [ "$(res "$a")" = "$w" ] || { ok=0; fail "control $a -> $(res "$a"), want $w"; }
done
[ "$(res ncpa.cpl)" = "program=sg-ncpa (else page=Network and Sharing Center)" ] || { ok=0; fail "ncpa.cpl"; }
[ $ok = 1 ] && pass "control.exe: .cpl names, keywords and canonical names open the right applet"
out=$(ctl --dump items)
[ "$(echo "$out" | grep -c '^category=')" = 7 ] && pass "Items: the seven categories" || fail "categories"

# ---- 9. The elevated half, end to end through the spool -----------------------------------------
if command -v python3 >/dev/null; then
    S="$T/spool"; B="$T/bin"; CALLS="$T/calls"
    mkdir -p "$S/requests" "$S/replies" "$B"; chmod 700 "$S/requests"; : > "$CALLS"
    for tool in hostnamectl useradd chpasswd gpasswd userdel timedatectl systemctl; do
        printf '#!/bin/sh\nin=$(cat 2>/dev/null)\nprintf "%%s %%s | %%s\\n" "%s" "$*" "$in" >> "%s"\n' "$tool" "$CALLS" > "$B/$tool"
    done
    printf '#!/bin/sh\n[ "$1" = group ] && { echo "$2:x:1:"; exit 0; }\nexit 2\n' > "$B/getent"
    printf '#!/bin/sh\nexit 1\n' > "$B/loginctl"
    chmod +x "$B"/*
    export SG_ADMIN_TEST=1 SG_ADMIN_SPOOL="$S" SG_ADMIN_PATH="$B" SG_ADMIN_SYSTEM_UID="$(id -u)" SG_ADMIN_HOSTS="$T/hosts"
    : > "$T/hosts"
    # what sg-admind.path does: run it when a request appears
    ( while :; do for f in "$S"/requests/*.req; do [ -e "$f" ] && python3 "$ADMIND" 2>>"$T/admind.log"; break; done; sleep 0.2; done ) &
    LOOP=$!
    r=$(ctl /admin-do hostname NEWPC)
    [ "$r" = "OK restart" ] && grep -q '^hostnamectl set-hostname NEWPC ' "$CALLS" \
        && pass "Elevated: the Control Panel renames the computer through sg-admind" || fail "rename: $r"
    r=$(ctl /admin-do hostname 'bad name'); [ "${r%% *}" = FAILED ] && pass "Elevated: sg-admind's refusal reaches the Control Panel" || fail "refusal: $r"
    r=$(printf 'Secr3t pw\n' | wine "$CTL" /admin-do user-add frank 'Frank Test' administrator 2>/dev/null | tr -d '\r')
    [ "$r" = OK ] && grep -q '^chpasswd  | frank:Secr3t pw$' "$CALLS" && grep -q -- '-G sgwine,sg-admins,sudo frank' "$CALLS" \
        && pass "Elevated: an account is created, the password passed on stdin, never argv" || fail "user-add: $r"
    ! grep -rq 'Secr3t' "$S" "$T/admind.log" 2>/dev/null && pass "Elevated: no password is left in the spool or the log" || fail "password left behind"
    r=$(SG_ADMIN_SPOOL="$T/nowhere" wine "$CTL" /admin-do update-check 2>/dev/null </dev/null | tr -d '\r')
    [ "${r%% *}" = FAILED ] && pass "Elevated: with no spool (not elevated), the request fails at once" || fail "no spool: $r"
fi

# ---- 10. The window ---------------------------------------------------------------------------------
if command -v Xvfb >/dev/null && command -v xdotool >/dev/null; then
    rm -f "/tmp/.X${DPY}-lock"
    Xvfb ":$DPY" -screen 0 1280x800x24 -ac >/dev/null 2>&1 & XP=$!; sleep 2
    export DISPLAY=":$DPY"
    wine "$CTL" >/dev/null 2>&1 &
    W=""; _w=0
    while [ $_w -lt 30 ]; do W=$(xdotool search --name '^Control Panel$' 2>/dev/null | head -1); [ -n "$W" ] && break; sleep 1; _w=$((_w+1)); done
    [ -n "$W" ] && pass "Window: the Control Panel appears" || fail "no Control Panel window"
    if [ -n "$W" ]; then
        sleep 2
        if command -v import >/dev/null; then
            n=$(import -window root png:- 2>/dev/null | convert png:- -format '%k' info: 2>/dev/null)
            [ "${n:-0}" -gt 20 ] && pass "Window: it paints ($n colors)" || fail "it does not paint ($n colors)"
        fi
        # the keyboard: focus starts on the first link ("Category"); Tab, Enter -> "Large icons"
        xdotool windowactivate --sync "$W" 2>/dev/null; xdotool key --window "$W" Tab 2>/dev/null; sleep 0.4; xdotool key --window "$W" Return 2>/dev/null
        _w=0; while [ $_w -lt 10 ]; do [ "$(xdotool getwindowname "$W" 2>/dev/null)" = "All Control Panel Items" ] && break; sleep 0.5; _w=$((_w+1)); done
        [ "$(xdotool getwindowname "$W" 2>/dev/null)" = "All Control Panel Items" ] \
            && pass "Window: Tab and Enter follow a link (All Control Panel Items)" || fail "keyboard: title is $(xdotool getwindowname "$W")"
        xdotool key --window "$W" alt+Left 2>/dev/null
        xdotool windowclose "$W" >/dev/null 2>&1
    fi
    # an elevated dialog, driven like a person: create an account
    if [ -n "$LOOP" ]; then
        : > "$CALLS"
        wine "$CTL" /admin user-add >/dev/null 2>&1 &
        D=""; _w=0
        while [ $_w -lt 20 ]; do D=$(xdotool search --name '^Create an account$' 2>/dev/null | head -1); [ -n "$D" ] && break; sleep 0.5; _w=$((_w+1)); done
        if [ -n "$D" ]; then
            xdotool windowactivate --sync "$D" 2>/dev/null; sleep 0.5
            xdotool type --window "$D" --delay 40 'gina' 2>/dev/null; xdotool key --window "$D" Tab 2>/dev/null
            xdotool type --window "$D" --delay 40 'gina test' 2>/dev/null; xdotool key --window "$D" Tab 2>/dev/null
            xdotool type --window "$D" --delay 40 'pw1' 2>/dev/null; xdotool key --window "$D" Tab 2>/dev/null
            xdotool type --window "$D" --delay 40 'pw1' 2>/dev/null; xdotool key --window "$D" Return 2>/dev/null
            _w=0; while [ $_w -lt 20 ]; do grep -q '^chpasswd' "$CALLS" && break; sleep 0.5; _w=$((_w+1)); done
            grep -q -- '-c gina test -G sgwine gina' "$CALLS" && grep -q '^chpasswd  | gina:pw1$' "$CALLS" \
                && pass "Dialog: 'Create an account' creates a standard account from what was typed" || fail "dialog: $(cat "$CALLS")"
        else fail "no 'Create an account' dialog"; fi
    fi
else
    echo "info  no X server; skipped the window checks"
fi

echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
