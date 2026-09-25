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
for tool in hostnamectl useradd userdel chpasswd gpasswd timedatectl systemctl sg-domain-join; do
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
chmod +x "$B"/*

export SG_ADMIN_TEST=1 SG_ADMIN_SPOOL="$S" SG_ADMIN_PATH="$B" SG_ADMIN_ZONEINFO="$T/zoneinfo" \
    SG_ADMIN_HOSTS="$T/hosts" SG_ADMIN_SYSTEM_UID="$(id -u)"
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

# --- time ---
: > "$CALLS"
r=$(ask timezone America/Denver)
grep -q '^timedatectl set-timezone America/Denver ' "$CALLS" && pass "sets the time zone" || fail "timezone: $r"
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

# --- what is not a request ---
r=$(ask reboot-now); case "$(first "$r")" in "FAILED Unknown"*) pass "refuses an unknown verb";; *) fail "unknown verb: $r";; esac
r=$(ask hostname a b); case "$(first "$r")" in "FAILED Malformed"*) pass "refuses the wrong number of fields";; *) fail "arity: $r";; esac
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
