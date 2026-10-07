#!/bin/sh
# Gate for sg-admind, the Control Panel's privileged half. Runs it unprivileged
# (SG_ADMIN_TEST=1) against a spool of its own, with stand-ins for the system
# tools that record how they were called, and checks each operation does what
# it should -- and, above all, what it refuses.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
ADMIND="$HERE/admin/sg-admind"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT INT TERM
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
command -v python3 >/dev/null || { echo "SKIP: no python3"; exit 77; }

S="$T/spool"; B="$T/bin"; CALLS="$T/calls"
mkdir -p "$S/requests" "$S/replies" "$B" "$T/zoneinfo/America"
chmod 700 "$S/requests"
: > "$CALLS"; : > "$T/zoneinfo/America/Denver"
printf '127.0.0.1\tlocalhost\n127.0.1.1\told-name\n' > "$T/hosts"

# the accounts the stand-in getent knows
cat > "$T/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/bash
sgsystem:x:990:990:SYSTEM:/var/lib/stained-glass:/usr/sbin/nologin
owner:x:1000:1000:Owner:/home/owner:/bin/bash
bob:x:1001:1001:Bob:/home/bob:/bin/bash
carol:x:1002:1002:Carol:/home/carol:/bin/bash
EOF
cat > "$T/group" <<'EOF'
sgwine:x:980:owner,bob,carol
sg-admins:x:981:owner
sudo:x:27:owner
EOF

# every stand-in records its name, arguments and stdin
for tool in hostnamectl useradd userdel chpasswd gpasswd timedatectl systemctl sg-domain-join apt-get runuser; do
    cat > "$B/$tool" <<EOF
#!/bin/sh
in=\$(cat 2>/dev/null)
printf '%s %s | %s\n' "$tool" "\$*" "\$in" >> "$CALLS"
EOF
done
# timedatectl also answers "is the clock synchronised?" (yes while $T/ntp-on exists)
cat > "$B/timedatectl" <<EOF
#!/bin/sh
in=\$(cat 2>/dev/null)
printf '%s %s | %s\n' timedatectl "\$*" "\$in" >> "$CALLS"
[ "\$1" = show ] && { [ -f "$T/ntp-on" ] && echo yes || echo no; }
exit 0
EOF
cat > "$B/getent" <<EOF
#!/bin/sh
f="$T/\$1"; [ -f "\$f" ] || exit 2
grep "^\$2:" "\$f" || exit 2
EOF
cat > "$B/loginctl" <<'EOF'
#!/bin/sh
# carol is signed in
[ "$2" = carol ] && { echo active; exit 0; }
exit 1
EOF
# dpkg-query -L: the package's files, one of them its registry defaults
cat > "$B/dpkg-query" <<'EOF'
#!/bin/sh
printf '/usr/share/doc/%s\n/usr/share/stained-glass/defaults.d/89-sg-office.reg\n/usr/share/stained-glass/defaults.d/README\n' "$2"
EOF
chmod +x "$B"/*

export SG_ADMIN_TEST=1 SG_ADMIN_SPOOL="$S" SG_ADMIN_PATH="$B" SG_ADMIN_ZONEINFO="$T/zoneinfo" \
    SG_ADMIN_HOSTS="$T/hosts" SG_ADMIN_SYSTEM_UID="$(id -u)" SG_ADMIN_TZ_AUTO="$T/tz-auto" SG_ADMIN_DEFENDER_CONF="$T/defender.conf" SG_ADMIN_DEFENDER_STATE="$T/def" SG_ADMIN_LOGIND_DROPIN="$T/logind.d/60-sg-power.conf"
# a fresh request id each time -- ask runs in $(...), so no shell variable
next_id() { n=$(($(cat "$T/n" 2>/dev/null || echo 0) + 1)); echo "$n" > "$T/n"; printf '%016x' "$n"; }
# ask VERB ARGS... -- file a request, run sg-admind, print the reply
ask() {
    id=$(next_id)
    printf '%s\n' "$@" > "$S/requests/.$id"
    mv "$S/requests/.$id" "$S/requests/$id.req"
    python3 "$ADMIND" 2>>"$T/log"
    cat "$S/replies/$id.rep" 2>/dev/null || echo "(no reply)"
}
first() { printf '%s\n' "$1" | head -1; }

r=$(ask ping); [ "$(printf "%s" "$r" | tr "\n" " ")" = "OK pong" ] && pass "ping answers" || fail "ping: $r"

# --- computer name ---
r=$(ask hostname new-pc)
[ "$(first "$r")" = OK ] && grep -q '^hostnamectl set-hostname new-pc ' "$CALLS" \
    && pass "renames the computer through hostnamectl" || fail "rename: $r"
grep -q "^127.0.1.1	new-pc$" "$T/hosts" && grep -q '^127.0.0.1	localhost$' "$T/hosts" \
    && pass "points 127.0.1.1 at the new name, leaving the rest of hosts" || fail "hosts: $(cat "$T/hosts")"
: > "$CALLS"
for bad in '-lead' 'has space' 'waytoolongcomputername' 'a;b' ''; do
    r=$(ask hostname "$bad")
    case "$(first "$r")" in "FAILED "*) ;; *) fail "accepted computer name '$bad'";; esac
done
[ ! -s "$CALLS" ] && pass "refuses bad computer names without touching the system" || fail "a bad name reached a tool: $(cat "$CALLS")"

# --- accounts ---
: > "$CALLS"
r=$(ask user-add dave 'Dave Example' administrator 'pa ss:w0rd')
[ "$(first "$r")" = OK ] && pass "creates an administrator" || fail "user-add: $r"
grep -q "^useradd -m -s /bin/bash -c Dave Example -G sgwine,sg-admins,sudo dave " "$CALLS" \
    && pass "... in sgwine, sg-admins and sudo, as sg-install makes the owner" || fail "useradd call: $(cat "$CALLS")"
grep -q '^chpasswd  | dave:pa ss:w0rd$' "$CALLS" && pass "... password set through chpasswd's stdin" || fail "chpasswd: $(cat "$CALLS")"
grep -q 'pa ss:w0rd' "$T/log" && fail "the password reached the log" || pass "the password is never logged"
: > "$CALLS"
r=$(ask user-add erin Erin standard secret)
grep -q -- "-G sgwine erin " "$CALLS" && pass "a standard account is only in sgwine" || fail "standard: $(cat "$CALLS")"
: > "$CALLS"
for bad in root sgsystem Bob '1abc' 'x;rm -rf' 'owner'; do
    r=$(ask user-add "$bad" X standard pw)
    case "$(first "$r")" in "FAILED "*) ;; *) fail "user-add accepted '$bad'";; esac
done
r=$(ask user-add frank 'Frank' superuser pw); case "$(first "$r")" in "FAILED "*) ;; *) fail "accepted type superuser";; esac
r=$(ask user-add frank 'a:b' standard pw); case "$(first "$r")" in "FAILED "*) ;; *) fail "accepted ':' in a full name";; esac
r=$(ask user-add frank 'Frank' standard ''); case "$(first "$r")" in "FAILED "*) ;; *) fail "accepted an empty password";; esac
[ ! -s "$CALLS" ] && pass "refuses reserved, existing and malformed accounts, types and passwords" || fail "reached a tool: $(cat "$CALLS")"

: > "$CALLS"
r=$(ask user-type owner standard)
case "$(first "$r")" in "FAILED "*"only administrator"*) pass "will not demote the only administrator";; *) fail "demoted the last admin: $r";; esac
r=$(ask user-remove owner delete)
case "$(first "$r")" in "FAILED "*"only administrator"*) pass "will not remove the only administrator";; *) fail "removed the last admin: $r";; esac
r=$(ask user-type bob administrator)
grep -q '^gpasswd -a bob sg-admins' "$CALLS" && grep -q '^gpasswd -a bob sudo' "$CALLS" \
    && pass "makes an account an administrator (sg-admins and sudo)" || fail "promote: $r $(cat "$CALLS")"
r=$(ask user-type root administrator)
case "$(first "$r")" in "FAILED "*) pass "will not touch a system account";; *) fail "changed root: $r";; esac
: > "$CALLS"
r=$(ask user-remove carol delete)
case "$(first "$r")" in "FAILED "*"signed in"*) pass "will not remove a signed-in account";; *) fail "removed carol: $r";; esac
r=$(ask user-remove bob keep)
grep -q '^userdel bob ' "$CALLS" && pass "removes an account, keeping its files when asked" || fail "remove keep: $(cat "$CALLS")"
r=$(ask user-remove bob delete)
grep -q '^userdel -r bob ' "$CALLS" && pass "... or deleting them" || fail "remove delete: $(cat "$CALLS")"
: > "$CALLS"
r=$(ask user-password bob 'n3w pass')
grep -q '^chpasswd  | bob:n3w pass$' "$CALLS" && pass "resets a password through chpasswd" || fail "password: $r"
# a person's own password: sg-password-change (a stand-in that records its
# NUL-separated stdin and answers as the real one; "wrong" is the wrong
# current password) -- not chpasswd, which cannot re-encrypt the keyring
cat > "$B/sg-password-change" <<EOF
#!/bin/sh
in=\$(tr '\\0' '|')
printf 'sg-password-change | %s\n' "\$in" >> "$CALLS"
case "\$in" in *"|wrong|"*) echo "FAIL current"; exit 1;; *"|short|"*) echo "FAIL You must choose a longer password."; exit 1;; esac
echo OK
EOF
chmod +x "$B/sg-password-change"
export SG_ADMIN_PWCHANGE="$B/sg-password-change"
: > "$CALLS"; : > "$T/log"
r=$(ask user-password-own bob 'old pw1' 'n3w pw2')
[ "$(first "$r")" = OK ] && grep -q '^sg-password-change | bob|old pw1|n3w pw2|$' "$CALLS" && ! grep -q '^chpasswd' "$CALLS" \
    && pass "changes a person's own password through sg-password-change (current and new), not chpasswd" || fail "own password: $r / $(cat "$CALLS")"
grep -q 'old pw1\|n3w pw2' "$T/log" && fail "a password of user-password-own reached the log" || pass "... neither password is logged"
r=$(ask user-password-own bob wrong 'n3w pw2')
[ "$(first "$r")" = "FAILED The current password is incorrect." ] && pass "a wrong current password is refused as such" || fail "wrong current: $r"
r=$(ask user-password-own bob 'old pw1' short)
case "$(first "$r")" in "FAILED "*"longer password"*) pass "PAM's reason for refusing a new password is shown";; *) fail "refused new: $r";; esac
: > "$CALLS"
r=$(ask user-password-own root 'old' 'new')
case "$(first "$r")" in "FAILED "*) [ ! -s "$CALLS" ] && pass "will not change a system account's password" || fail "root reached a tool";; *) fail "own password for root: $r";; esac
r=$(ask user-password-own bob '' 'new')
case "$(first "$r")" in "FAILED "*) pass "an empty current password is refused";; *) fail "empty current: $r";; esac
# an sg-session without the helper: the reset, as before
r=$(SG_ADMIN_PWCHANGE="$T/none" ask user-password-own bob 'old pw1' 'n3w pw2')
grep -q '^chpasswd  | bob:n3w pw2$' "$CALLS" && pass "without sg-password-change it falls back to the reset" || fail "fallback: $r / $(cat "$CALLS")"
# MUTANT NOKEYRING: user-password-own as a plain reset (chpasswd) -- the
# keyring would stay on the old password
sed 's/^    "user-password-own": (op_user_password_own, 3),$/    "user-password-own": (lambda a: op_user_password([a[0], a[2]]), 3),/' "$ADMIND" > "$T/mut-nokeyring"
grep -q 'op_user_password_own, 3' "$T/mut-nokeyring" && fail "the NOKEYRING mutant did not apply"
: > "$CALLS"
id=$(next_id); printf 'user-password-own\nbob\nold pw1\nn3w pw2\n' > "$S/requests/.m"; mv "$S/requests/.m" "$S/requests/$id.req"
python3 "$T/mut-nokeyring" 2>>"$T/log"
grep -q '^sg-password-change | bob|old pw1|n3w pw2|$' "$CALLS" && ! grep -q '^chpasswd' "$CALLS" \
    && fail "NOKEYRING not detected" || pass "MUTANT NOKEYRING (a reset instead) is caught"

# --- time ---
: > "$CALLS"
r=$(ask timezone America/Denver)
grep -q '^timedatectl set-timezone America/Denver ' "$CALLS" && pass "sets the time zone" || fail "timezone: $r"
[ "$(cat "$T/tz-auto" 2>/dev/null)" = off ] && pass "...and a zone chosen by hand turns 'automatically' off" || fail "tz-auto after a zone: $(cat "$T/tz-auto" 2>/dev/null)"
r=$(ask timezone-auto on)
[ "$(first "$r")" = OK ] && [ "$(cat "$T/tz-auto")" = on ] && pass "turns 'Set time zone automatically' on" || fail "timezone-auto on: $r"
# what the power button and the lid do (Power & sleep): logind's, a drop-in of our own
r=$(ask power-buttons ignore suspend hibernate)
[ "$(first "$r")" = OK ] && grep -qx 'HandleLidSwitch=ignore' "$T/logind.d/60-sg-power.conf" \
    && grep -qx 'HandleLidSwitchExternalPower=suspend' "$T/logind.d/60-sg-power.conf" && grep -qx 'HandlePowerKey=hibernate' "$T/logind.d/60-sg-power.conf" \
    && grep -qx '\[Login\]' "$T/logind.d/60-sg-power.conf" && pass "sets what the lid (on battery, plugged in) and the power button do" || fail "power-buttons: $r $(cat "$T/logind.d/60-sg-power.conf" 2>&1)"
r=$(ask power-buttons ignore suspend 'poweroff
HandleSuspendKey=ignore')
case "$(first "$r")" in "FAILED "*) pass "...only the actions logind knows (no lines slipped in)" ;; *) fail "accepted a made-up action: $r" ;; esac
r=$(ask defender off)
[ "$(first "$r")" = OK ] && grep -qx "enabled=0" "$T/defender.conf" && pass "turns SG Defender off" || fail "defender off: $r"
r=$(ask defender on)
[ "$(first "$r")" = OK ] && grep -qx "enabled=1" "$T/defender.conf" && pass "...and on" || fail "defender on: $r"
r=$(ask defender sometimes)
case "$(first "$r")" in "FAILED "*) pass "...only on or off" ;; *) fail "accepted defender sometimes" ;; esac
# quarantine: what sg-defender left, restored as its owner or deleted
Q="$T/def/quarantine"; mkdir -p "$Q" "$T/def/notices/$(id -u)" "$T/dl"
quar() {   # ID NAME CONTENT
    printf '%s' "$3" > "$Q/$1.bin"
    printf '{"id": "%s", "path": "%s/dl/%s", "uid": %s, "signature": "Test", "sha256": "%s", "mode": 493}' \
        "$1" "$T" "$2" "$(id -u)" "$(printf '%s' "$3" | sha256sum | cut -d' ' -f1)" > "$Q/$1.json"
    echo '{}' > "$T/def/notices/$(id -u)/$1.json"
}
quar 20261003-160447-aaaaaa setup.exe "infected-ish"
r=$(ask defender-restore 20261003-160447-aaaaaa)
[ "$(first "$r")" = OK ] && [ "$(cat "$T/dl/setup.exe" 2>/dev/null)" = infected-ish ] && [ -x "$T/dl/setup.exe" ] \
    && [ ! -e "$Q/20261003-160447-aaaaaa.bin" ] && [ ! -e "$T/def/notices/$(id -u)/20261003-160447-aaaaaa.json" ] \
    && pass "Restore puts a quarantined file back where it was, runnable as it was, and clears it" || fail "restore: $r"
grep -q "^$(printf infected-ish | sha256sum | cut -d' ' -f1) setup.exe" "$T/def/allowed" 2>/dev/null \
    && pass "...and sg-defender is told to let that file be" || fail "not on the allowed list"
quar 20261003-160447-bbbbbb setup.exe "second"
r=$(ask defender-restore 20261003-160447-bbbbbb)
[ "$(cat "$T/dl/setup (restored).exe" 2>/dev/null)" = second ] && [ "$(cat "$T/dl/setup.exe")" = infected-ish ] \
    && pass "...beside a file of the same name, not over it" || fail "restore beside: $r"
quar 20261003-160447-cccccc planted.exe "third"; echo keep > "$T/victim"; ln -s "$T/victim" "$T/dl/planted.exe"
r=$(ask defender-restore 20261003-160447-cccccc)
[ "$(cat "$T/victim")" = keep ] && pass "...never through a link planted where it was" || fail "followed a planted link"
quar 20261003-160447-dddddd gone.exe "fourth"
r=$(ask defender-delete 20261003-160447-dddddd)
[ "$(first "$r")" = OK ] && [ ! -e "$Q/20261003-160447-dddddd.bin" ] && [ ! -e "$T/dl/gone.exe" ] \
    && pass "Delete removes a quarantined file for good" || fail "delete: $r"
r=$(ask defender-restore ../../etc/passwd)
case "$(first "$r")" in "FAILED "*) pass "...only quarantine ids" ;; *) fail "accepted a path as a quarantine id" ;; esac
r=$(ask timezone-auto maybe)
case "$(first "$r")" in "FAILED "*) pass "...only on or off" ;; *) fail "accepted timezone-auto maybe" ;; esac
: > "$CALLS"
for bad in '../../etc/passwd' 'America/Nowhere' 'America' '/etc/passwd' 'a b'; do
    r=$(ask timezone "$bad")
    case "$(first "$r")" in "FAILED "*) ;; *) fail "accepted zone '$bad'";; esac
done
[ ! -s "$CALLS" ] && pass "refuses zones that are not in zoneinfo" || fail "bad zone reached timedatectl"
r=$(ask ntp on); grep -q '^timedatectl set-ntp true' "$CALLS" && pass "turns time synchronisation on" || fail "ntp: $r"
: > "$CALLS"
r=$(ask time '2026-09-25 14:30:00')
grep -q '^timedatectl set-time 2026-09-25 14:30:00 ' "$CALLS" && pass "sets the clock by hand" || fail "time: $r"
: > "$CALLS"
for bad in '2026-02-30 10:00:00' '2026-09-25 25:00:00' 'now' '2026-09-25T14:30:00' '1999-01-01 00:00:00' '2026-09-25 14:30:00 --adjust'; do
    r=$(ask time "$bad")
    case "$(first "$r")" in "FAILED "*) ;; *) fail "accepted time '$bad'";; esac
done
grep -q 'set-time' "$CALLS" && fail "a bad time reached timedatectl" || pass "refuses times that are not a date and time"
touch "$T/ntp-on"; : > "$CALLS"
r=$(ask time '2026-09-25 14:30:00')
case "$(first "$r")" in "FAILED "*) if grep -q 'set-time' "$CALLS"; then fail "set the clock while synchronised"; else pass "refuses a hand-set clock while synchronised"; fi;; *) fail "time with ntp on: $r";; esac
rm -f "$T/ntp-on"

# --- domain, updates ---
: > "$CALLS"
r=$(ask join-domain sgtest.lan administrator 10.0.2.15 'Dom@inPw')
grep -q '^sg-domain-join --domain sgtest.lan --user administrator --password-stdin --dc 10.0.2.15 | Dom@inPw$' "$CALLS" \
    && pass "joins a domain, the password on stdin" || fail "join: $r $(cat "$CALLS")"
r=$(ask join-domain 'not a domain' administrator '' pw); case "$(first "$r")" in "FAILED "*) pass "refuses a malformed domain";; *) fail "join accepted a bad realm";; esac
r=$(ask update-check); grep -q '^systemctl start --no-block sg-update-prepare.service' "$CALLS" && pass "starts the update check" || fail "update: $r"

# --- where updates come from ---
A="$T/sources.list.d"; mkdir -p "$A"; export SG_APT_SOURCES="$A"
printf 'Types: deb\nURIs: https://deb.debian.org/debian\nSuites: trixie\nComponents: main\n' > "$A/debian.sources"
r=$(ask source-add extras https://repo.example.org/debian "stable" "main contrib")
f="$A/sg-user-extras.sources"
{ [ "$(first "$r")" = OK ] && grep -qx 'URIs: https://repo.example.org/debian' "$f" && grep -qx 'Components: main contrib' "$f" \
    && grep -qx 'Enabled: yes' "$f"; } && pass "adds a source (deb822, sg-user-NAME.sources)" || fail "source-add: $r $(cat "$f" 2>/dev/null)"
r=$(ask source-add extras https://other.example.org/ stable main); case "$(first "$r")" in "FAILED "*) pass "not twice under one name";; *) fail "source-add twice: $r";; esac
r=$(ask source-add ../evil https://a.example/ stable main); case "$(first "$r")" in "FAILED "*) pass "refuses a name that is a path";; *) fail "bad name: $r";; esac
r=$(ask source-add x 'file:///etc/shadow' stable main); case "$(first "$r")" in "FAILED "*) pass "refuses an address that is not the web";; *) fail "bad uri: $r";; esac
r=$(ask source-add y 'https://a.example/ x' stable main); case "$(first "$r")" in "FAILED "*) pass "refuses an address with a space (a second field)";; *) fail "uri with space: $r";; esac
r=$(ask source-add z https://a.example/ 'stable
Signed-By: /x' main); case "$(first "$r")" in "FAILED "*) pass "refuses a field smuggled into a list";; *) fail "smuggled field: $r";; esac
r=$(ask source-enable debian.sources no)
{ [ "$(first "$r")" = OK ] && grep -qx 'Enabled: no' "$A/debian.sources" && grep -qx 'Suites: trixie' "$A/debian.sources"; } \
    && pass "turns a source off, the rest of it kept" || fail "source-enable: $r $(cat "$A/debian.sources")"
r=$(ask source-enable debian.sources yes); grep -qx 'Enabled: yes' "$A/debian.sources" && ! grep -qx 'Enabled: no' "$A/debian.sources" \
    && pass "and on again" || fail "source-enable yes: $(cat "$A/debian.sources")"
r=$(ask source-enable ../../etc/passwd no); case "$(first "$r")" in "FAILED "*) pass "only files in the sources folder";; *) fail "enable path: $r";; esac
r=$(ask source-remove extras); { [ "$(first "$r")" = OK ] && [ ! -e "$f" ]; } && pass "removes a source added here" || fail "source-remove: $r"
r=$(ask source-remove debian); case "$(first "$r")" in "FAILED "*) pass "and only those";; *) fail "removed a system source: $r";; esac
[ -f "$A/debian.sources" ] || fail "debian.sources is gone"

# --- SG Store: system packages (apt-install, deb-install) ---
# a stand-in apt-get that reports progress on APT::Status-Fd, looks at the
# progress file sg-admind publishes meanwhile, and "installs" (the section's
# own; the others' stand-ins come back after it)
cp "$B/apt-get" "$T/apt-get.others"; cp "$B/dpkg-query" "$T/dpkg-query.others"
cat > "$B/apt-get" <<EOF
#!/bin/sh
fd=; last=
for a in "\$@"; do case \$a in APT::Status-Fd=*) fd=\${a#APT::Status-Fd=} ;; esac; last=\$a; done
printf 'apt-get %s\n' "\$*" >> "$CALLS"
case " \$* " in *" update "*) exit 0 ;; esac
[ -n "\$fd" ] && eval "printf 'pmstatus:x:50:Unpacking the thing\n' >&\$fd"
sleep 0.6
cat "$S"/replies/*.progress >> "$T/progress-seen" 2>/dev/null
if [ -f "\$last" ]; then printf '%s %s\n' "\$(dpkg-deb -f "\$last" Package)" "\$(dpkg-deb -f "\$last" Version)" >> "$T/installed"
else printf '%s 9.9\n' "\$last" >> "$T/installed"; fi
EOF
cat > "$B/dpkg-query" <<EOF
#!/bin/sh
# -L: the package's files, one of them its registry defaults
[ "\$1" = -L ] && { printf '/usr/share/doc/%s\n/usr/share/stained-glass/defaults.d/89-sg-office.reg\n/usr/share/stained-glass/defaults.d/README\n' "\$2"; exit 0; }
eval "pkg=\\\${\$#}"
v=\$(sed -n "s/^\$pkg //p" "$T/installed" 2>/dev/null | tail -1)
[ -n "\$v" ] || exit 1
printf 'installed %s' "\$v"
EOF
chmod +x "$B/apt-get" "$B/dpkg-query"
DEBS="$T/debs"; mkdir -p "$DEBS" "$T/work"; chmod 700 "$DEBS"
export SG_ADMIN_DEBS="$DEBS" SG_ADMIN_WORK="$T/work"
: > "$CALLS"; : > "$T/installed"
r=$(ask apt-install gimp)
{ [ "$(first "$r")" = OK ] && grep -q '^apt-get .* install gimp$' "$CALLS" && printf '%s' "$r" | grep -q 'gimp 9.9'; } \
    && pass "apt-install runs apt-get install and answers the installed version" || fail "apt-install: $r / $(cat "$CALLS")"
grep -q '^PROGRESS [0-9]' "$T/progress-seen" 2>/dev/null && grep -q 'Unpacking the thing' "$T/progress-seen" \
    && pass "and publishes apt's progress while it runs" || fail "progress: $(cat "$T/progress-seen" 2>/dev/null)"
[ -z "$(ls "$S"/replies/*.progress 2>/dev/null)" ] && pass "the progress file goes with the answer" || fail "progress left behind"
: > "$CALLS"
for bad in 'gimp; reboot' '-oAPT::X=1' '../gimp' 'Gimp' ''; do
    r=$(ask apt-install "$bad"); case "$(first "$r")" in "FAILED "*) ;; *) fail "apt-install '$bad': $r" ;; esac
done
[ ! -s "$CALLS" ] && pass "refuses what is not a package name, apt never runs" || fail "a bad name reached apt: $(cat "$CALLS")"
grep -q '^runuser ' "$CALLS" && fail "a package not ours was registered in the Windows side" || true
# one of our own (SG Office): installed, then registered as SYSTEM at once
reg_cases() { # ADMIND -- one word per case
    : > "$CALLS"; : > "$T/installed"
    id=$(next_id); printf 'apt-install\nsg-office\n' > "$S/requests/.r"; mv "$S/requests/.r" "$S/requests/$id.req"
    python3 "$1" 2>>"$T/log"; r=$(cat "$S/replies/$id.rep" 2>/dev/null)
    reg_call=$(grep '^runuser ' "$CALLS")
    { [ "$(first "$r")" = OK ] && grep -q '^apt-get .* install sg-office$' "$CALLS" \
      && printf '%s' "$reg_call" | grep -q '^runuser -u sgsystem -- env HOME=/var/lib/stained-glass XDG_RUNTIME_DIR=/run/stained-glass sh -c .*wine reg import.* sg-register /usr/share/stained-glass/defaults.d/89-sg-office.reg |' \
      && ! printf '%s' "$reg_call" | grep -q 'README\|sg-prefix-init'; } && echo registered || echo not-registered
    : > "$CALLS"
    id=$(next_id); printf 'apt-install\ngimp\n' > "$S/requests/.r"; mv "$S/requests/.r" "$S/requests/$id.req"
    python3 "$1" 2>>"$T/log"
    grep -q '^runuser ' "$CALLS" && echo registers-others || echo ours-only
}
# shellcheck disable=SC2046
set -- $(reg_cases "$ADMIND")
[ "${1:-}" = registered ] && pass "apt-install of our own package (sg-office) imports its registry defaults as the machine account" \
    || fail "sg-office not registered: $(tr '\n' '|' < "$CALLS")"
[ "${2:-}" = ours-only ] && pass "and only ours: another package is not registered" || fail "gimp was registered"
sed 's/^        register_defaults(pkg)$/        pass/' "$ADMIND" > "$T/mut-noregister"
grep -q '^        register_defaults(pkg)$' "$T/mut-noregister" && fail "the NOREGISTER mutant did not apply"
# shellcheck disable=SC2046
set -- $(reg_cases "$T/mut-noregister")
[ "${1:-}" != registered ] && pass "MUTANT NOREGISTER leaves SG Office unregistered until a restart (gate catches it)" || fail "NOREGISTER not detected"

P="$T/pkg"; mkdir -p "$P/DEBIAN"
printf 'Package: sg-gate-hello\nVersion: 1.2-3\nArchitecture: all\nMaintainer: Gate <g@example.org>\nDescription: gate\n' > "$P/DEBIAN/control"
dpkg-deb --root-owner-group --build "$P" "$T/hello.deb" >/dev/null 2>&1 || fail "the gate's .deb does not build"
stage() { cp "$T/hello.deb" "$DEBS/$1"; }
# deb_file_request ADMIND ID VERSION [link] -- stage, file the request, run
deb_request() {
    stage "$2.deb"
    [ -n "${4:-}" ] && ln "$DEBS/$2.deb" "$T/extra-link"     # a second name: not SYSTEM's own file
    printf 'deb-install\n%s\nsg-gate-hello\n%s\n' "$2.deb" "$3" > "$S/requests/.r"; mv "$S/requests/.r" "$S/requests/$2.req"
    python3 "$1" 2>>"$T/log"; rm -f "$T/extra-link"
    cat "$S/replies/$2.rep" 2>/dev/null
}
deb_cases() { # the sg-admind to test -- prints one word per case
    : > "$CALLS"; : > "$T/installed"
    id=$(next_id); r=$(deb_request "$1" "$id" 1.2-3)
    { [ "$(first "$r")" = OK ] && grep -q "^apt-get .* install $T/work/sg-deb-[^/]*/package.deb\$" "$CALLS" \
      && [ ! -e "$DEBS/$id.deb" ] && [ -z "$(ls -A "$T/work")" ]; } && echo installs || echo "no-install"
    # the file changed after it was shown: another version
    : > "$CALLS"
    id=$(next_id); r=$(deb_request "$1" "$id" 1.2-4); rm -f "$DEBS/$id.deb"
    case "$(first "$r")" in "FAILED The file changed"*) grep -q ' install ' "$CALLS" && echo installed-changed || echo refuses-changed ;;
                            *) echo installed-changed ;; esac
    # a staged file with another name as well: not SYSTEM's own
    : > "$CALLS"
    id=$(next_id); r=$(deb_request "$1" "$id" 1.2-3 link); rm -f "$DEBS/$id.deb"
    case "$(first "$r")" in "FAILED "*) grep -q ' install ' "$CALLS" && echo installed-foreign || echo refuses-foreign ;;
                            *) echo installed-foreign ;; esac
}
# shellcheck disable=SC2046
set -- $(deb_cases "$ADMIND")
[ "${1:-}" = installs ] && pass "deb-install takes SYSTEM's staged .deb, apt installs its own copy, both copies go" || fail "deb-install: ${1:-} $(tail -2 "$T/log")"
[ "${2:-}" = refuses-changed ] && pass "a package that is not the version shown is refused, apt never runs" || fail "changed: ${2:-}"
[ "${3:-}" = refuses-foreign ] && pass "a staged file that is not SYSTEM's own is refused" || fail "foreign: ${3:-}"
: > "$CALLS"
for bad in '../hello.deb' 'hello.deb' "$(next_id).deb/x"; do
    r=$(ask deb-install "$bad" sg-gate-hello 1.2-3); case "$(first "$r")" in "FAILED "*) ;; *) fail "deb-install '$bad': $r" ;; esac
done
id=$(next_id); ln -s "$T/hello.deb" "$DEBS/$id.deb"
r=$(ask deb-install "$id.deb" sg-gate-hello 1.2-3); rm -f "$DEBS/$id.deb"
case "$(first "$r")" in "FAILED "*) ;; *) fail "deb-install through a link: $r" ;; esac
id=$(next_id); printf 'not a package\n' > "$DEBS/$id.deb"
r=$(ask deb-install "$id.deb" sg-gate-hello 1.2-3)
case "$(first "$r")" in "FAILED The file is not a Linux"*) ;; *) fail "deb-install of a non-package: $r" ;; esac
[ ! -s "$CALLS" ] && pass "refuses other names, links and files that are not packages, apt never runs" || fail "reached apt: $(cat "$CALLS")"
# mutants: without the version check, without the owner check -- the gate must catch both
sed 's/^        if (pkg, ver) != (want_pkg, want_ver):/        if False:/' "$ADMIND" > "$T/mut-nocheck"
sed "s/^            if not stat.S_ISREG(st.st_mode) or st.st_nlink != 1 or st.st_uid != system_uid():  # SYSTEM's own\$/            if False:/" "$ADMIND" > "$T/mut-noowner"
[ "$(grep -c 'if False:' "$T/mut-nocheck")" = 1 ] && [ "$(grep -c 'if False:' "$T/mut-noowner")" = 1 ] || fail "the mutants did not apply"
# shellcheck disable=SC2046
set -- $(deb_cases "$T/mut-nocheck")
[ "${2:-}" != refuses-changed ] && pass "MUTANT NOVERSIONCHECK installs a changed package (gate catches it)" || fail "NOVERSIONCHECK not detected"
# shellcheck disable=SC2046
set -- $(deb_cases "$T/mut-noowner")
[ "${3:-}" != refuses-foreign ] && pass "MUTANT NOOWNER installs a file that is not SYSTEM's own (gate catches it)" || fail "NOOWNER not detected"
# Steam's kind of package: it asks for 32-bit libraries (steam-libs-i386) and
# brings its own apt source, where those libraries are. i386 goes on first;
# after the package, its missing Recommends come from the new source.
cat > "$B/dpkg" <<EOF
#!/bin/sh
case "\$1" in
--print-architecture) echo amd64 ;;
--print-foreign-architectures) cat "$T/foreign" 2>/dev/null ;;
--add-architecture) printf 'dpkg --add-architecture %s\n' "\$2" >> "$CALLS"; echo "\$2" >> "$T/foreign" ;;
esac
EOF
chmod +x "$B/dpkg"
P="$T/steampkg"; mkdir -p "$P/DEBIAN" "$P/etc/apt/sources.list.d"
printf 'Package: sg-gate-steam\nVersion: 1:1.0-1\nArchitecture: amd64\nMaintainer: Gate <g@example.org>\nDepends: curl, python3 (>= 3.4)\nRecommends: sg-gate-libs-amd64, sg-gate-libs-i386, sudo, xdg-utils | sg-gate-other\nDescription: gate\n' > "$P/DEBIAN/control"
echo 'deb https://repo.example.org stable main' > "$P/etc/apt/sources.list.d/sg-gate-steam.list"
dpkg-deb --root-owner-group --build "$P" "$T/steam.deb" >/dev/null 2>&1 || fail "the gate's Steam-like .deb does not build"
steam_request() { # ADMIND -- prints the reply
    id=$(next_id); cp "$T/steam.deb" "$DEBS/$id.deb"
    printf 'deb-install\n%s\nsg-gate-steam\n1:1.0-1\n' "$id.deb" > "$S/requests/.r"; mv "$S/requests/.r" "$S/requests/$id.req"
    python3 "$1" 2>>"$T/log"; cat "$S/replies/$id.rep" 2>/dev/null
}
steam_cases() { # ADMIND -- one word per case
    : > "$CALLS"; printf 'sudo 1.9\nxdg-utils 1.2\n' > "$T/installed"; : > "$T/foreign"
    r=$(steam_request "$1")
    add=$(grep -n 'add-architecture i386' "$CALLS" | cut -d: -f1 | head -1)
    deb=$(grep -n ' install .*/package.deb$' "$CALLS" | cut -d: -f1 | head -1)
    { [ "$(first "$r")" = OK ] && [ -n "$add" ] && [ -n "$deb" ] && [ "$add" -lt "$deb" ]; } && echo i386-first || echo no-i386
    { [ -n "$deb" ] && grep -q ' install sg-gate-libs-amd64$' "$CALLS" && grep -q ' install sg-gate-libs-i386:i386$' "$CALLS" \
      && [ "$(sed -n "$deb,\$p" "$CALLS" | grep -c ' update$')" -ge 1 ] && ! grep -q ' install sudo$\| install sg-gate-other$' "$CALLS"; } \
        && echo recommends || echo no-recommends
    : > "$CALLS"; printf 'sudo 1.9\nxdg-utils 1.2\n' > "$T/installed"
    r=$(steam_request "$1")
    { [ "$(first "$r")" = OK ] && ! grep -q add-architecture "$CALLS"; } && echo once || echo twice
}
# shellcheck disable=SC2046
set -- $(steam_cases "$ADMIND")
[ "${1:-}" = i386-first ] && pass "a package that asks for i386 libraries turns i386 on before it installs" || fail "i386: ${1:-} $(cat "$CALLS")"
[ "${2:-}" = recommends ] && pass "then its missing Recommends come from the source it added (the i386 one as :i386; ones met are left)" || fail "recommends: ${2:-} $(cat "$CALLS")"
[ "${3:-}" = once ] && pass "i386 already on: left as it is" || fail "turned on twice: $(cat "$CALLS")"
: > "$CALLS"; : > "$T/installed"
id=$(next_id); r=$(deb_request "$ADMIND" "$id" 1.2-3)
{ [ "$(first "$r")" = OK ] && ! grep -q 'add-architecture' "$CALLS" && [ "$(grep -c ' install ' "$CALLS")" = 1 ]; } \
    && pass "a plain package: no i386, nothing more installed" || fail "plain: $(cat "$CALLS")"
# a recommended library that cannot be had: the package is still installed, and the answer says what is missing
cp "$B/apt-get" "$T/apt-get.store"
awk '{print} /^case " \$\* " in \*" update "\*\) exit 0 ;; esac$/ {print "case \" $* \" in *\" sg-gate-libs-i386:i386 \"*) exit 100 ;; esac"}' "$T/apt-get.store" > "$B/apt-get"
grep -q 'exit 100' "$B/apt-get" || fail "the failing apt-get stand-in did not apply"
printf 'sudo 1.9\nxdg-utils 1.2\n' > "$T/installed"; : > "$T/foreign"
r=$(steam_request "$ADMIND")
{ [ "$(first "$r")" = OK ] && printf '%s\n' "$r" | grep -qx 'Not installed: sg-gate-libs-i386'; } \
    && pass "a recommended library that fails is named, the package still installed" || fail "failed recommend: $r"
cp "$T/apt-get.store" "$B/apt-get"
sed 's/^        if wants_i386(rel) and "i386" not in foreign_archs():$/        if False:/' "$ADMIND" > "$T/mut-noi386"
sed 's/^    failed = install_recommends(rel, 60, 38) if sources else \[\]$/    failed = []/' "$ADMIND" > "$T/mut-norec"
[ "$(grep -c 'if False:' "$T/mut-noi386")" = 1 ] && ! grep -q 'failed = install_recommends(rel, 60, 38) if sources' "$T/mut-norec" || fail "the i386 mutants did not apply"
# shellcheck disable=SC2046
set -- $(steam_cases "$T/mut-noi386")
[ "${1:-}" != i386-first ] && pass "MUTANT NOI386 leaves i386 off (gate catches it)" || fail "NOI386 not detected"
# shellcheck disable=SC2046
set -- $(steam_cases "$T/mut-norec")
[ "${2:-}" != recommends ] && pass "MUTANT NORECOMMENDS skips Steam's libraries (gate catches it)" || fail "NORECOMMENDS not detected"
# Steam from SG Store (apt-install steam-launcher): Valve's apt source and key
# go in first (identical to the files Valve's package ships), i386 before the
# install, then the 32-bit libraries the launcher recommends
cat > "$B/apt-cache" <<'EOF'
#!/bin/sh
[ "$1" = show ] || exit 1
eval "pkg=\${$#}"
case "$pkg" in
steam-launcher) printf 'Package: steam-launcher\nVersion: 1:1.0.0.87\nDepends: apt (>= 1.1), curl, python3 (>= 3.4)\nRecommends: steam-libs-amd64, steam-libs-i386, sudo, xdg-utils | steamos-base-files\nDescription: Launcher for the Steam software distribution service\n' ;;
*) printf 'Package: %s\nVersion: 9.9\nDepends: libc6\nDescription: x\n' "$pkg" ;;
esac
EOF
chmod +x "$B/apt-cache"
mkdir -p "$T/keyrings"; export SG_ADMIN_VENDORS="$HERE/admin/apt-vendors" SG_ADMIN_KEYRINGS="$T/keyrings"
store_steam() { # ADMIND -- one word per case
    : > "$CALLS"; printf 'sudo 1.9\nxdg-utils 1.2\n' > "$T/installed"; : > "$T/foreign"
    rm -f "$A/steam-stable.list" "$T/keyrings/steam.gpg"
    id=$(next_id); printf 'apt-install\nsteam-launcher\n' > "$S/requests/.r"; mv "$S/requests/.r" "$S/requests/$id.req"
    python3 "$1" 2>>"$T/log"; r=$(cat "$S/replies/$id.rep" 2>/dev/null)
    cmp -s "$A/steam-stable.list" "$HERE/admin/apt-vendors/steam-stable.list" && cmp -s "$T/keyrings/steam.gpg" "$HERE/admin/apt-vendors/steam.gpg" \
        && echo source || echo no-source
    add=$(grep -n 'add-architecture i386' "$CALLS" | cut -d: -f1 | head -1)
    ins=$(grep -n ' install steam-launcher$' "$CALLS" | cut -d: -f1 | head -1)
    { [ "$(first "$r")" = OK ] && [ -n "$add" ] && [ -n "$ins" ] && [ "$add" -lt "$ins" ]; } && echo i386-first || echo no-i386
    { grep -q ' install steam-libs-amd64$' "$CALLS" && grep -q ' install steam-libs-i386:i386$' "$CALLS" && ! grep -q ' install sudo$' "$CALLS"; } \
        && echo libs || echo no-libs
}
# shellcheck disable=SC2046
set -- $(store_steam "$ADMIND")
[ "${1:-}" = source ] && pass "Steam from SG Store: Valve's apt source and key are put in place (Valve's own files)" || fail "steam source: ${1:-} $(ls "$A" "$T/keyrings")"
[ "${2:-}" = i386-first ] && pass "and i386 turned on before the launcher installs" || fail "steam i386: ${2:-} $(tr '\n' '|' < "$CALLS")"
[ "${3:-}" = libs ] && pass "and its 32-bit libraries come with it" || fail "steam libs: ${3:-} $(tr '\n' '|' < "$CALLS")"
: > "$CALLS"; rm -f "$A/steam-stable.list"
r=$(ask apt-install gimp)
{ [ "$(first "$r")" = OK ] && [ ! -e "$A/steam-stable.list" ] && ! grep -q add-architecture "$CALLS"; } \
    && pass "another package brings no source and no i386" || fail "gimp: $(tr '\n' '|' < "$CALLS")"
sed 's/^    vendor = add_vendor_source(pkg)$/    vendor = False/' "$ADMIND" > "$T/mut-novendor"
grep -q 'vendor = False' "$T/mut-novendor" || fail "the NOVENDOR mutant did not apply"
# shellcheck disable=SC2046
set -- $(store_steam "$T/mut-novendor")
[ "${1:-}" != source ] && [ "${3:-}" != libs ] && pass "MUTANT NOVENDOR: no Valve source, no libraries (gate catches it)" || fail "NOVENDOR not detected"
# the browsers' Linux builds (SG Store's drop-down): the maker's source and key
# (the files its package writes itself), and Mozilla's pin -- only firefox
# from Mozilla's repository, so Debian's firefox-esr stays Debian's
mkdir -p "$T/prefs"; export SG_ADMIN_PREFERENCES="$T/prefs"
browser() { # ADMIND PACKAGE FILE:DIR... -- "ok" when the reply is OK and every file is the maker's
    rm -f "$A"/google-chrome.sources "$A"/microsoft-edge.list "$A"/mozilla.list "$T"/keyrings/* "$T"/prefs/*
    id=$(next_id); printf 'apt-install\n%s\n' "$2" > "$S/requests/.r"; mv "$S/requests/.r" "$S/requests/$id.req"
    python3 "$1" 2>>"$T/log"; r=$(cat "$S/replies/$id.rep" 2>/dev/null)
    [ "$(first "$r")" = OK ] || { echo "reply:$(first "$r")"; return; }
    pkg=$2; shift 2
    for fd in "$@"; do cmp -s "${fd#*:}/${fd%%:*}" "$HERE/admin/apt-vendors/${fd%%:*}" || { echo "missing:${fd%%:*}"; return; }; done
    echo ok
}
r=$(browser "$ADMIND" google-chrome-stable "google-chrome.sources:$A" "google-chrome.gpg:$T/keyrings")
[ "$r" = ok ] && pass "Chrome for Linux: Google's source and key, as Chrome writes them" || fail "chrome: $r"
r=$(browser "$ADMIND" microsoft-edge-stable "microsoft-edge.list:$A" "microsoft-edge.gpg:$T/keyrings")
[ "$r" = ok ] && pass "Edge for Linux: Microsoft's source and key" || fail "edge: $r"
r=$(browser "$ADMIND" firefox "mozilla.list:$A" "packages.mozilla.org.asc:$T/keyrings" "mozilla.pref:$T/prefs")
[ "$r" = ok ] && pass "Firefox for Linux: Mozilla's source, key and pin" || fail "firefox: $r"
grep -q '^Package: firefox firefox-l10n-\*$' "$HERE/admin/apt-vendors/mozilla.pref" && grep -q '^Pin-Priority: 100$' "$HERE/admin/apt-vendors/mozilla.pref" \
    && pass "the pin takes only firefox from Mozilla (everything else at 100, below Debian's)" || fail "mozilla.pref"
# Mozilla's repository already configured by hand (Mozilla's own
# instructions: mozilla.sources with its key in /etc/apt/keyrings): the Store
# reuses it -- a second entry naming another keyring makes apt refuse every
# source ("Conflicting values set for option Signed-By", David's VM
# 2026-10-06). sg-session's sg-apt-sources does it; mutant: without it.
TOOL=${SG_APT_SOURCES_TOOL:-}
for c in "$HERE/../sg-session/lib/sg-apt-sources" "$HOME/Stained-Glass-OS/sg-session/lib/sg-apt-sources" /usr/lib/stained-glass/sg-apt-sources; do
    [ -n "$TOOL" ] || { [ -x "$c" ] && TOOL=$c; }
done
own_mozilla() { # TOOL -- "reused" when no mozilla.list was added beside the user's .sources
    rm -f "$A"/*; mkdir -p "$T/etckeys"; cp "$HERE/admin/apt-vendors/packages.mozilla.org.asc" "$T/etckeys/"
    printf 'Types: deb\nURIs: https://packages.mozilla.org/apt\nSuites: mozilla\nComponents: main\nSigned-By: %s/etckeys/packages.mozilla.org.asc\n' "$T" > "$A/mozilla.sources"
    id=$(next_id); printf 'apt-install\nfirefox\n' > "$S/requests/.r"; mv "$S/requests/.r" "$S/requests/$id.req"
    SG_ADMIN_APT_SOURCES_TOOL=$1 python3 "$ADMIND" 2>>"$T/log"; r=$(cat "$S/replies/$id.rep" 2>/dev/null)
    { [ "$(first "$r")" = OK ] && [ ! -e "$A/mozilla.list" ] && [ -e "$A/mozilla.sources" ]; } && echo reused || echo "added:$(ls "$A" | tr '\n' ' ')"
}
if [ -n "$TOOL" ]; then
    r=$(own_mozilla "$TOOL")
    [ "$r" = reused ] && pass "Firefox for Linux with Mozilla's repository already set up by hand: reused, no conflicting second entry" || fail "own mozilla source: $r"
    r=$(own_mozilla /nonexistent)
    [ "$r" != reused ] && pass "MUTANT no sg-apt-sources: a second Mozilla entry is added (gate catches it)" || fail "NO_ADOPT mutant not detected"
    rm -f "$A"/*
else echo "SKIP  sg-session's sg-apt-sources not found (SG_APT_SOURCES_TOOL): the reuse case"; fi
r=$(browser "$T/mut-novendor" firefox "mozilla.list:$A")
[ "$r" != ok ] && pass "MUTANT NOVENDOR: no Mozilla source (gate catches it)" || fail "NOVENDOR not detected for firefox"
rm -f "$B/apt-cache"
rm -f "$B/dpkg"
cp "$T/apt-get.others" "$B/apt-get"; cp "$T/dpkg-query.others" "$B/dpkg-query"

# --- SG Store: Uninstall (apt-remove) ---
# stand-ins: apt-get (-s shows what would go, from $T/rdeps: "PKG REMOVED..."
# lines; remove takes packages out of $T/installed), dpkg-query (status,
# version, Essential/Priority, Depends, and -L: SG Office's real registry
# files), runuser (keeps the unregistration .reg it is handed)
cp "$B/apt-get" "$T/apt-get.others"; cp "$B/dpkg-query" "$T/dpkg-query.others"; cp "$B/runuser" "$T/runuser.others"
mkdir -p "$T/defaults"; cp "$HERE"/office/defaults/89-sg-office*.reg "$T/defaults/"
cat > "$B/apt-get" <<EOF
#!/bin/sh
printf 'apt-get %s\n' "\$*" >> "$CALLS"
sim=; act=; pkgs=
for a in "\$@"; do case \$a in -s) sim=1 ;; -*|*=*) ;; remove|autoremove|install|update) act=\$a ;; *) pkgs="\$pkgs \$a" ;; esac; done
if [ -n "\$sim" ]; then
    if [ "\$act" = autoremove ]; then for p in \$(cat "$T/auto" 2>/dev/null); do grep -q "^\$p " "$T/installed" && echo "Remv \$p [1.0]"; done; exit 0; fi
    for p in \$pkgs; do echo "Remv \$p [1.0]"; sed -n "s/^\$p //p" "$T/rdeps" 2>/dev/null | tr ' ' '\n' | sed '/^$/d; s/^/Remv /; s/$/ [1.0]/'; done
    exit 0
fi
[ "\$act" = remove ] && for p in \$pkgs; do grep -v "^\$p " "$T/installed" > "$T/installed.n"; mv "$T/installed.n" "$T/installed"; done
exit 0
EOF
cat > "$B/dpkg-query" <<EOF
#!/bin/sh
[ "\$1" = -L ] && { ls "$T/defaults"/*.reg; echo /usr/share/doc/\$2; exit 0; }
eval "pkg=\\\${\$#}"
case "\$*" in
  *Essential*) case \$pkg in base-files) echo "yes required" ;; *) echo "no optional" ;; esac; exit 0 ;;
  *Depends*) sed -n "s/^\$pkg //p" "$T/deps" 2>/dev/null; exit 0 ;;
esac
v=\$(sed -n "s/^\$pkg //p" "$T/installed" 2>/dev/null | tail -1)
[ -n "\$v" ] || exit 1
printf 'installed %s' "\$v"
EOF
cat > "$B/runuser" <<EOF
#!/bin/sh
printf 'runuser %s\n' "\$*" >> "$CALLS"
for a in "\$@"; do case \$a in *.reg) [ -f "\$a" ] && cp "\$a" "$T/unreg.reg" ;; esac; done
exit 0
EOF
chmod +x "$B/apt-get" "$B/dpkg-query" "$B/runuser"
export SG_ADMIN_DEFAULTS="$T/defaults"
remove_cases() { # ADMIND -- one word per case
    # a Linux app: removed, with the library it alone brought (auto, its dependency)
    printf 'gimp 2.10\nlibgimp 2.10\nlibc6 2.41\nsg-shell 0.1.0-86\n' > "$T/installed"
    printf 'gimp libgimp, libc6\n' > "$T/deps"; printf 'libgimp\n' > "$T/auto"; : > "$T/rdeps"; : > "$CALLS"
    id=$(next_id); printf 'apt-remove\ngimp\n' > "$S/requests/.r"; mv "$S/requests/.r" "$S/requests/$id.req"
    python3 "$1" 2>>"$T/log"; r=$(cat "$S/replies/$id.rep" 2>/dev/null)
    { [ "$(first "$r")" = OK ] && grep -q '^apt-get .* remove gimp$' "$CALLS" && grep -q '^apt-get .* remove libgimp$' "$CALLS" \
      && ! grep -q 'libc6$' "$CALLS" && ! grep -q '^gimp ' "$T/installed"; } && echo removed || echo not-removed
    # what would take the system with it: refused, apt never removes
    printf 'tool 1.0\nsg-shell 0.1.0-86\n' > "$T/installed"; printf 'tool sg-shell\n' > "$T/rdeps"; : > "$T/deps"; : > "$CALLS"
    id=$(next_id); printf 'apt-remove\ntool\n' > "$S/requests/.r"; mv "$S/requests/.r" "$S/requests/$id.req"
    python3 "$1" 2>>"$T/log"; r=$(cat "$S/replies/$id.rep" 2>/dev/null)
    { case "$(first "$r")" in "FAILED "*"sg-shell"*) true ;; *) false ;; esac; } && ! grep -q '^apt-get [^-]*-y.* remove' "$CALLS" \
      && echo guarded || echo unguarded
    # SG Office: unregistered from the Windows side as the machine account first, then removed with its editors
    printf 'sg-office 0.1.0-86\nsg-office-editors 8.2\nsg-shell 0.1.0-86\n' > "$T/installed"
    printf 'sg-office sg-shell (= 0.1.0-86), wine-sg | wine, sg-office-editors\n' > "$T/deps"; printf 'sg-office-editors\n' > "$T/auto"
    : > "$T/rdeps"; : > "$CALLS"; rm -f "$T/unreg.reg"
    id=$(next_id); printf 'apt-remove\nsg-office\n' > "$S/requests/.r"; mv "$S/requests/.r" "$S/requests/$id.req"
    python3 "$1" 2>>"$T/log"; r=$(cat "$S/replies/$id.rep" 2>/dev/null)
    unreg_line=$(grep -n '^runuser -u sgsystem .*sg-unregister .*89-sg-office' "$CALLS" | head -1 | cut -d: -f1)
    remove_line=$(grep -n '^apt-get -y .* remove sg-office$' "$CALLS" | head -1 | cut -d: -f1)
    { [ "$(first "$r")" = OK ] && [ -n "$unreg_line" ] && [ -n "$remove_line" ] && [ "$unreg_line" -lt "$remove_line" ] \
      && grep -q '^apt-get .* remove sg-office-editors$' "$CALLS" \
      && grep -qF '[-HKEY_LOCAL_MACHINE\Software\Classes\SGOffice.Document.12]' "$T/unreg.reg" \
      && grep -qF '[-HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\App Paths\sg-documents.exe]' "$T/unreg.reg" \
      && grep -qF '"SG Office"=-' "$T/unreg.reg" && grep -qF '"SGOffice.Document.12"=-' "$T/unreg.reg" \
      && ! grep -qF '[-HKEY_LOCAL_MACHINE\Software\Classes\.docx]' "$T/unreg.reg"; } && echo unregistered || echo not-unregistered
}
# shellcheck disable=SC2046
set -- $(remove_cases "$ADMIND")
[ "${1:-}" = removed ] && pass "apt-remove uninstalls a Linux app, with the dependency it alone brought (not shared libraries)" \
    || fail "apt-remove gimp: ${1:-} $(tr '\n' '|' < "$CALLS")"
[ "${2:-}" = guarded ] && pass "apt-remove refuses what would remove the system's own packages (sg-shell); apt removes nothing" \
    || fail "apt-remove guard: ${2:-} $(tr '\n' '|' < "$CALLS")"
[ "${3:-}" = unregistered ] && pass "apt-remove of SG Office takes its ProgIDs, App Paths and values out of the Windows side (not .docx itself), then it and its editors go" \
    || fail "apt-remove sg-office: ${3:-} $(tr '\n' '|' < "$CALLS") $(head -c 600 "$T/unreg.reg" 2>/dev/null)"
: > "$CALLS"; printf 'base-files 13\nsg-shell 0.1.0-86\nwine-sg 10.0-133\n' > "$T/installed"; : > "$T/rdeps"
for bad in 'gimp; reboot' '-oAPT::X=1' 'base-files' 'sg-shell' 'wine-sg' 'notinstalled'; do
    r=$(ask apt-remove "$bad"); case "$(first "$r")" in "FAILED "*) ;; *) fail "apt-remove '$bad': $r" ;; esac
done
grep ' remove ' "$CALLS" | grep -qv -- ' -s ' && fail "a refused removal reached apt: $(cat "$CALLS")" \
    || pass "refuses names, the base system (essential, ours, Wine) and what is not installed; apt removes nothing"
sed 's/^    for name in \[pkg\] + gone:$/    for name in []:/' "$ADMIND" > "$T/mut-noguard"
grep -q 'for name in \[\]:' "$T/mut-noguard" || fail "the NOGUARD mutant did not apply"
# shellcheck disable=SC2046
set -- $(remove_cases "$T/mut-noguard")
[ "${2:-}" = unguarded ] && pass "MUTANT NOGUARD removes sg-shell with a tool (gate catches it)" || fail "NOGUARD not detected"
sed 's/^        unregister_defaults(pkg)$/        pass/' "$ADMIND" > "$T/mut-nounreg"
grep -q '^        unregister_defaults(pkg)$' "$T/mut-nounreg" && fail "the NOUNREG mutant did not apply"
# shellcheck disable=SC2046
set -- $(remove_cases "$T/mut-nounreg")
[ "${3:-}" != unregistered ] && pass "MUTANT NOUNREG leaves SG Office's file types behind (gate catches it)" || fail "NOUNREG not detected"
unset SG_ADMIN_DEFAULTS
cp "$T/apt-get.others" "$B/apt-get"; cp "$T/dpkg-query.others" "$B/dpkg-query"; cp "$T/runuser.others" "$B/runuser"

# --- automatic sign-in and the kiosk app (Settings > Accounts > Kiosk) ---
# greetd's configuration as the image ships it
cat > "$T/greetd.toml" <<'TOML'
# greetd: the Windows-style login screen (ADR 0008).

[terminal]
vt = 1

[default_session]
command = "/usr/lib/stained-glass/sg-login-ui"
user = "sggreet"
TOML
cp "$T/greetd.toml" "$T/greetd.orig"
export SG_ADMIN_GREETD_CONF="$T/greetd.toml" SG_ADMIN_AUTOLOGON_CONF="$T/etc-sg/autologon.conf"
# what greetd will read: TOML, its initial session's user and command (tomllib: python 3.11)
toml_get() {   # FILE -> "initial_user|initial_command|default_command|default_user"
    python3 - "$1" <<'PY'
import sys, tomllib
c = tomllib.load(open(sys.argv[1], "rb"))
i = c.get("initial_session", {}); d = c.get("default_session", {})
print("%s|%s|%s|%s" % (i.get("user", ""), i.get("command", ""), d.get("command", ""), d.get("user", "")))
PY
}
autologon_checks() {   # ADMIND -> sets K_* results
    K_ON=no; K_OFF=no
    id=$(next_id); printf 'autologon\nbob\n' > "$S/requests/.k"; mv "$S/requests/.k" "$S/requests/$id.req"
    python3 "$1" 2>>"$T/log"
    [ "$(toml_get "$T/greetd.toml" 2>/dev/null)" = "bob|/usr/bin/sg-session-start|/usr/lib/stained-glass/sg-login-ui|sggreet" ] && K_ON=yes
    id=$(next_id); printf 'autologon\n\n' > "$S/requests/.k"; mv "$S/requests/.k" "$S/requests/$id.req"
    python3 "$1" 2>>"$T/log"
    cmp -s "$T/greetd.toml" "$T/greetd.orig" && [ ! -e "$T/etc-sg/autologon.conf" ] && K_OFF=yes
    cp "$T/greetd.orig" "$T/greetd.toml"; rm -f "$T/etc-sg/autologon.conf"
}
if python3 -c 'import tomllib' 2>/dev/null; then
    r=$(ask autologon bob)
    [ "$(first "$r")" = OK ] && [ "$(toml_get "$T/greetd.toml")" = "bob|/usr/bin/sg-session-start|/usr/lib/stained-glass/sg-login-ui|sggreet" ] \
        && grep -qx 'user=bob' "$T/etc-sg/autologon.conf" \
        && pass "automatic sign-in: greetd's initial session signs bob in; the sign-in screen stays the default session" \
        || fail "autologon bob: $r / $(cat "$T/greetd.toml")"
    [ "$(stat -c %a "$T/greetd.toml")" = 644 ] && [ "$(stat -c %a "$T/etc-sg/autologon.conf")" = 644 ] \
        && pass "...both files readable by everyone (greetd, the session, Settings), written whole" || fail "modes: $(stat -c '%n %a' "$T/greetd.toml" "$T/etc-sg/autologon.conf")"
    grep -qi 'password' "$T/greetd.toml" "$T/etc-sg/autologon.conf" && fail "a password is kept" || pass "...and no password is kept anywhere"
    r=$(ask autologon bob)
    [ "$(grep -c '^\[initial_session\]' "$T/greetd.toml")" = 1 ] && pass "...asked again: still one initial session" || fail "twice: $(cat "$T/greetd.toml")"
    r=$(ask kiosk Sonos 'C:\Program Files (x86)\Sonos\Sonos.exe' '--kiosk "x"' 'C:\Program Files (x86)\Sonos' '')
    [ "$(first "$r")" = OK ] && grep -qxF 'kiosk-app=C:\Program Files (x86)\Sonos\Sonos.exe' "$T/etc-sg/autologon.conf" \
        && grep -qxF 'kiosk-args=--kiosk "x"' "$T/etc-sg/autologon.conf" && grep -qx 'kiosk-name=Sonos' "$T/etc-sg/autologon.conf" \
        && grep -qx 'user=bob' "$T/etc-sg/autologon.conf" && pass "the kiosk app (a Windows program) is kept for that account" || fail "kiosk: $r / $(cat "$T/etc-sg/autologon.conf")"
    cp "$T/etc-sg/autologon.conf" "$T/kiosk.before"
    r=$(ask kiosk Sonos 'C:\Sonos.exe' 'a
user=root' '' '')
    case "$(first "$r")" in "FAILED "*) cmp -s "$T/kiosk.before" "$T/etc-sg/autologon.conf" && pass "...no line can be slipped into it" || fail "changed by a refused request";; *) fail "accepted a line break: $r";; esac
    r=$(ask kiosk Both 'C:\a.exe' '' '' "$T/app.desktop")
    case "$(first "$r")" in "FAILED "*) pass "...one program, not two";; *) fail "accepted two: $r";; esac
    r=$(ask kiosk Shell 'C:\a.bat' '' '' '')
    case "$(first "$r")" in "FAILED "*) pass "...a program or a shortcut to one, not a script";; *) fail "accepted .bat: $r";; esac
    r=$(ask kiosk Gone '' '' '' "$T/none.desktop")
    case "$(first "$r")" in "FAILED "*) pass "...a Linux app that is not there is refused";; *) fail "accepted a missing .desktop: $r";; esac
    printf '[Desktop Entry]\nType=Application\nName=App\nExec=true\n' > "$T/app.desktop"
    r=$(ask kiosk App '' '' '' "$T/app.desktop")
    [ "$(first "$r")" = OK ] && grep -qx "kiosk-desktop=$T/app.desktop" "$T/etc-sg/autologon.conf" && ! grep -q '^kiosk-app=' "$T/etc-sg/autologon.conf" \
        && pass "a Linux app as the kiosk app (its .desktop file), replacing the Windows one" || fail "kiosk desktop: $r / $(cat "$T/etc-sg/autologon.conf")"
    r=$(ask kiosk '' '' '' '' '')
    [ "$(first "$r")" = OK ] && ! grep -q '^kiosk-' "$T/etc-sg/autologon.conf" && grep -qx 'user=bob' "$T/etc-sg/autologon.conf" \
        && pass "no kiosk app: removed, automatic sign-in stays" || fail "kiosk off: $r / $(cat "$T/etc-sg/autologon.conf")"
    r=$(ask kiosk App '' '' '' "$T/app.desktop")
    r=$(ask autologon carol)
    grep -qx 'user=carol' "$T/etc-sg/autologon.conf" && ! grep -q '^kiosk-' "$T/etc-sg/autologon.conf" \
        && [ "$(toml_get "$T/greetd.toml" | cut -d'|' -f1)" = carol ] \
        && pass "another account: its own sign-in, without bob's kiosk app" || fail "switch: $(cat "$T/etc-sg/autologon.conf")"
    for who in root sgsystem nosuch 'bob"'; do
        r=$(ask autologon "$who")
        case "$(first "$r")" in "FAILED "*) ;; *) fail "signed $who in automatically: $r";; esac
    done
    [ "$(toml_get "$T/greetd.toml" | cut -d'|' -f1)" = carol ] && pass "never a system account, an unknown one or a made-up name" || fail "refusals changed it"
    r=$(ask autologon '')
    [ "$(first "$r")" = OK ] && cmp -s "$T/greetd.toml" "$T/greetd.orig" && [ ! -e "$T/etc-sg/autologon.conf" ] \
        && pass "off: greetd's configuration exactly as before, nothing kept" || fail "off: $r / $(diff "$T/greetd.orig" "$T/greetd.toml")"
    r=$(ask kiosk App '' '' '' "$T/app.desktop")
    case "$(first "$r")" in "FAILED "*) [ ! -e "$T/etc-sg/autologon.conf" ] && pass "no kiosk app without automatic sign-in";; *) fail "kiosk without autologon: $r";; esac
    # an initial session somebody else configured is not ours to replace
    printf '\n[initial_session]\ncommand = "sway"\nuser = "john"\n' >> "$T/greetd.toml"; cp "$T/greetd.toml" "$T/greetd.foreign"
    r=$(ask autologon bob)
    case "$(first "$r")" in "FAILED "*) cmp -s "$T/greetd.toml" "$T/greetd.foreign" && pass "an initial session not ours is left alone (refused)";; *) fail "replaced a foreign initial session: $r";; esac
    cp "$T/greetd.orig" "$T/greetd.toml"
    # MUTANTS: no initial session written / turning it off leaves it
    autologon_checks "$ADMIND"
    [ "$K_ON $K_OFF" = "yes yes" ] && pass "(the mutants' baseline holds)" || fail "baseline: $K_ON $K_OFF"
    sed 's/"\[initial_session\]",/"[initial_sessions]",/' "$ADMIND" > "$T/mut-noinitial"
    autologon_checks "$T/mut-noinitial"
    [ "$K_ON" = no ] && pass "MUTANT AUTOLOGON_NOINITIAL (greetd never signs in) is caught" || fail "AUTOLOGON_NOINITIAL not detected"
    sed 's/^        greetd_autologon("")$/        pass/' "$ADMIND" > "$T/mut-nooff"
    autologon_checks "$T/mut-nooff"
    [ "$K_OFF" = no ] && pass "MUTANT AUTOLOGON_NOOFF (turning it off leaves it on) is caught" || fail "AUTOLOGON_NOOFF not detected"
else
    echo "SKIP  automatic sign-in: no tomllib (python3 < 3.11)"
fi

# --- what is not a request ---
r=$(ask reboot-now); case "$(first "$r")" in "FAILED Unknown"*) pass "refuses an unknown verb";; *) fail "unknown verb: $r";; esac
r=$(ask hostname a b); case "$(first "$r")" in "FAILED Malformed"*) pass "refuses the wrong number of fields";; *) fail "arity: $r";; esac
# ... into the RUNNING shared wineserver: its socket is under /tmp, so the
# service must not have a private /tmp (it had: the import went to a second
# server of its own, and SG Store said "restart, then install it again")
private_tmp() { grep -Eiq '^[[:space:]]*PrivateTmp[[:space:]]*=[[:space:]]*(yes|true|on|1|disconnected)' "$1"; }
private_tmp "$HERE/admin/sg-admind.service" && fail "sg-admind.service has a private /tmp: apt-install's registration misses the running wineserver" \
    || pass "sg-admind shares /tmp, so its registration reaches the running shared wineserver"
sed 's/^TimeoutStartSec=/PrivateTmp=yes\nTimeoutStartSec=/' "$HERE/admin/sg-admind.service" > "$T/mut-privtmp.service"
private_tmp "$T/mut-privtmp.service" && pass "MUTANT PRIVATETMP (the old unit) is caught" || fail "PRIVATETMP mutant not detected"

# not SYSTEM's: the file's owner is checked
: > "$CALLS"
id=$(next_id); printf 'update-check\n' > "$S/requests/$id.req"
SG_ADMIN_SYSTEM_UID=12345 python3 "$ADMIND" 2>>"$T/log"
[ ! -e "$S/replies/$id.rep" ] && [ ! -s "$CALLS" ] && [ ! -e "$S/requests/$id.req" ] \
    && pass "ignores (and clears) a request not owned by SYSTEM" || fail "acted on a foreign request"
# a link is never followed
printf 'update-check\n' > "$T/elsewhere"
id=$(next_id); ln -s "$T/elsewhere" "$S/requests/$id.req"
python3 "$ADMIND" 2>>"$T/log"
[ ! -e "$S/replies/$id.rep" ] && [ ! -s "$CALLS" ] && pass "does not follow a symbolic link planted as a request" || fail "followed a link"

echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
