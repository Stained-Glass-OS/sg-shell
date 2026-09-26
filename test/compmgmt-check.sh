#!/bin/sh
# shellcheck disable=SC2046,SC2155,SC2086
# Gate for Computer Management (compmgmt.msc, sg-mmc), under Xvfb on a shell
# desktop, reading what it shows from SG_MMC_DUMP:
#
#   - compmgmt.msc opens with Windows' tree: System Tools (Event Viewer,
#     Shared Folders, Local Users and Groups, Device Manager), Storage (Disk
#     Management), Services and Applications (Services, and the Stained Glass
#     system services), in that order
#   - each console works inside it: Services lists the SCM's services,
#     Event Viewer's Application log opens, Device Manager lists devices,
#     Disk Management lists disks
#   - Local Users and Groups (sg-sysinfo users/groups over a made-up passwd
#     and group file): the accounts, the administrator and the SYSTEM
#     account marked; Groups shows sg-admins as Administrators and sgwine as
#     Users, with their members; everything read-only
#   - Shared Folders > Shares (a stand-in testparm): the Samba shares with
#     their folders as Windows paths; with no Samba, "Samba is not installed"
#
# Screenshots: build/compmgmt-*.png. SG_MMC_EXE, SG_WINE_DIR, SG_SYSINFO as
# the other console gates. Mutant: -DSG_MUTANT_WINNAME (Linux group names).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_MMC_EXE:-$HERE/build/sg-mmc64.exe}"
OUT="$HERE/build"
SYSINFO="${SG_SYSINFO:-$HERE/../sg-session/bin/sg-sysinfo}"
[ -x "$SYSINFO" ] || SYSINFO=/usr/bin/sg-sysinfo
SYSINFO=$(readlink -f "$SYSINFO")
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import python3; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] && [ -x "$SYSINFO" ] || { echo "SKIP: $EXE or sg-sysinfo missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-compmgmt.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

# made-up accounts and Samba configuration
cat > "$T/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/bash
alice:x:1001:1001:Alice Liddell:/home/alice:/bin/bash
bob:x:1002:1002:Bob Admin:/home/bob:/bin/bash
sgsystem:x:1003:1003::/var/lib/stained-glass:/usr/sbin/nologin
EOF
cat > "$T/group" <<'EOF'
root:x:0:
alice:x:1001:
bob:x:1002:
sgsystem:x:1003:
sgwine:x:1500:alice,bob,sgsystem
sg-admins:x:1501:bob
EOF
mkdir -p "$T/srv/public"
cat > "$T/testparm" <<EOF
#!/bin/sh
printf '[global]\n\tworkgroup = SGTEST\n\n[Public]\n\tcomment = Everyone files\n\tpath = $T/srv/public\n\tguest ok = Yes\n\tread only = No\n\n[print\$]\n\tpath = /var/lib/samba/printers\n'
EOF
chmod 755 "$T/testparm"

Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$T/dpy" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/dpy" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/dpy")" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WBIN:$PATH" SG_SYSINFO="$SYSINFO" SG_PASSWD_FILE="$T/passwd" SG_GROUP_FILE="$T/group" SG_TESTPARM="$T/testparm" SG_NET=
DUMP="$T/dump.txt"
wine wineboot --init >/dev/null 2>&1; "$WSERVER" -w
C="$WINEPREFIX/drive_c"
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
windump=$(wine winepath -w "$DUMP" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\mmc.exe' /ve /d "$winexe"
for f in "$HERE"/theme/5[0-2]-*.reg; do wine regedit /S "Z:$(printf %s "$f" | tr / '\\')" >/dev/null 2>&1; done
"$WSERVER" -w
export SG_MMC_DUMP="$windump"
wine explorer /desktop=shell,1280x800 > "$T/explorer.out" 2>&1 &
sleep 4

d() { [ -f "$DUMP" ] && tr -d '\r' < "$DUMP"; }
wait_dump() { i=0; while ! d | grep -Eq "$1" && [ $i -lt $(( ${2:-10} * 2 )) ]; do sleep 0.5; i=$((i + 1)); done; d | grep -Eq "$1"; }
tree_xy() { d | awk -v n="$1" '$1 == "TREE" { t = $0; sub(/^TREE [0-9]+ -?[0-9]+ -?[0-9]+ (\* )?/, "", t); if (t == n && $3 >= 0) { print $3, $4; exit } }'; }
row() { d | awk -F'\t' -v n="$1" '$1 ~ /^ROW / && $2 == n { print; exit }'; }
click() { [ $# -ge 2 ] || return 0; xdotool mousemove "$1" "$2" click 1; sleep 1; }
go() {  # select a tree item; expand it (Right) so its children get positions
    click $(tree_xy "$1"); xdotool key Right; sleep 0.8
}
shot() { import -window root "$OUT/compmgmt-$1.png" 2>/dev/null; }

if [ -f "$C/windows/system32/compmgmt.msc" ]; then wine start compmgmt.msc >/dev/null 2>&1 &
else wine "$winexe" compmgmt >/dev/null 2>&1 & fi
wait_dump '^CONSOLE compmgmt' 30 && wait_dump '^TREE' 5 && pass "compmgmt.msc opens Computer Management" || fail "Computer Management did not open"
tree=$(d | awk '$1 == "TREE" { t = $0; sub(/^TREE [0-9]+ -?[0-9]+ -?[0-9]+ (\* )?/, "", t); printf "%s%s|", $2, t }')
case "$tree" in
    "0Computer Management (Local)|1System Tools|2Event Viewer (Local)|"*"2Shared Folders|"*"2Local Users and Groups|"*"2Device Manager|1Storage|2Disk Management|1Services and Applications|2Services (Local)|2Stained Glass System Services|")
        pass "Windows' tree: System Tools, Storage, Services and Applications, and their consoles" ;;
    *) fail "tree: $tree" ;;
esac
d | awk -F'\t' '$1 ~ /^ROW / { printf "%s|", $2 }' | grep -q '^System Tools|Storage|Services and Applications|$' \
    && pass "the root lists its folders in the console's order" || fail "root rows: $(d | awk -F'\t' '$1 ~ /^ROW / { printf "%s|", $2 }')"
shot open

click $(tree_xy 'Services (Local)')
wait_dump '^NODE Services \(Local\)' 5 && wait_dump '	Plug and Play Service	' 10 && pass "Services lists the SCM's services" || fail "services: $(d | grep -E '^(NODE|ROWS)')"
click $(tree_xy 'Device Manager')
wait_dump '^NODE Device Manager' 5 && wait_dump '^DEV ' 20 && pass "Device Manager lists devices" || fail "devices: $(d | grep -cE '^DEV ')"
click $(tree_xy 'Disk Management')
wait_dump '^NODE Disk Management' 5 && wait_dump '^DISK ' 20 && pass "Disk Management lists disks" || fail "disks: $(d | grep -E '^(NODE|DISK)')"
shot disks
go 'Event Viewer (Local)'; go 'System Logs'
click $(tree_xy 'Application')
wait_dump '^NODE Application' 5 && wait_dump '^LOG Application' 5 && pass "Event Viewer's Application log opens" || fail "event viewer: $(d | grep -E '^(NODE|LOG)')"

go 'Local Users and Groups'
click $(tree_xy 'Users')
if wait_dump '^NODE Users' 5 && wait_dump '	alice	' 10; then
    case "$(row alice)" in *"	Alice Liddell	"*"	Yes	No") pass "Users: alice, Alice Liddell, Windows, not an administrator" ;; *) fail "alice: $(row alice)" ;; esac
    case "$(row bob)" in *"	Yes	Yes") pass "Users: bob is an administrator" ;; *) fail "bob: $(row bob)" ;; esac
    case "$(row sgsystem)" in *"SYSTEM"*) pass "Users: sgsystem is the SYSTEM account" ;; *) fail "sgsystem: $(row sgsystem)" ;; esac
else fail "users: $(d | grep -E '^(NODE|ROWS|EMPTY)')"; fi
d | grep -q '^VERB row' && fail "an account offers actions (it must be read-only)" || pass "accounts are read-only (no actions)"
shot users
click $(tree_xy 'Groups')
if wait_dump '^NODE Groups' 5 && wait_dump '	Administrators	' 10; then
    case "$(row Administrators)" in *"	bob	sg-admins") pass "Groups: Administrators (sg-admins) with bob" ;; *) fail "Administrators: $(row Administrators)" ;; esac
    case "$(row Users)" in *"	alice bob sgsystem	sgwine"|*"	alice,bob,sgsystem	sgwine") pass "Groups: Users (sgwine) with its members" ;; *) fail "Users: $(row Users)" ;; esac
else fail "groups: $(d | grep -E '^(NODE|ROWS|EMPTY|ROW)' | head -5)"; fi
shot groups

go 'Shared Folders'
click $(tree_xy 'Shares')
if wait_dump '^NODE Shares' 5 && wait_dump '	Public	' 10; then
    want=$(wine winepath -w "$T/srv/public" 2>/dev/null | tr -d '\r')
    case "$(row Public)" in *"	$want	Disk	Everyone files	Read/Write, guests") pass "Shares: Public at $want, read/write, guests" ;;
                           *) fail "Public: $(row Public) (want $want)" ;; esac
else fail "shares: $(d | grep -E '^(NODE|ROWS|EMPTY)')"; fi
shot shares
wine taskkill /f /im sg-mmc64.exe >/dev/null 2>&1; sleep 1

# no Samba
rm -f "$DUMP"
SG_TESTPARM= wine "$winexe" compmgmt >/dev/null 2>&1 &
wait_dump '^TREE' 30
go 'System Tools'; go 'Shared Folders'
click $(tree_xy 'Shares')
wait_dump '^EMPTY Samba is not installed' 10 && pass "without Samba: 'Samba is not installed'" || fail "no samba: $(d | grep -E '^(NODE|EMPTY)')"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
