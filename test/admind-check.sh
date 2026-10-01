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
printf '/usr/share/doc/%s\n/usr/share/stained-glass/defaults.d/89-sg-office.reg\n/usr/share/stained-glass/defaults.d/README\n/usr/libexec/stained-glass/shell/sg-office-setup64.exe\n' "$2"
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
[ "$(grep -c 'if False:' "$T/mut-noi386")" = 1 ] && ! grep -q 'failed = install_recommends' "$T/mut-norec" || fail "the i386 mutants did not apply"
# shellcheck disable=SC2046
set -- $(steam_cases "$T/mut-noi386")
[ "${1:-}" != i386-first ] && pass "MUTANT NOI386 leaves i386 off (gate catches it)" || fail "NOI386 not detected"
# shellcheck disable=SC2046
set -- $(steam_cases "$T/mut-norec")
[ "${2:-}" != recommends ] && pass "MUTANT NORECOMMENDS skips Steam's libraries (gate catches it)" || fail "NORECOMMENDS not detected"
rm -f "$B/dpkg"
cp "$T/apt-get.others" "$B/apt-get"; cp "$T/dpkg-query.others" "$B/dpkg-query"

# --- what is not a request ---
r=$(ask reboot-now); case "$(first "$r")" in "FAILED Unknown"*) pass "refuses an unknown verb";; *) fail "unknown verb: $r";; esac
r=$(ask hostname a b); case "$(first "$r")" in "FAILED Malformed"*) pass "refuses the wrong number of fields";; *) fail "arity: $r";; esac
# package-install: our own packages only; installed, then registered as SYSTEM
: > "$CALLS"
r=$(ask package-install sg-office)
reg_call=$(grep '^runuser ' "$CALLS")
if [ "$(first "$r")" = OK ] && grep -q '^apt-get -q -y .* install sg-office' "$CALLS" \
    && printf '%s' "$reg_call" | grep -q '^runuser -u sgsystem -- env HOME=/var/lib/stained-glass XDG_RUNTIME_DIR=/run/stained-glass sh -c .*wine reg import.* sg-register /usr/share/stained-glass/defaults.d/89-sg-office.reg |' \
    && ! printf '%s' "$reg_call" | grep -q 'README\|sg-office-setup64\|sg-prefix-init'; then
    pass "package-install installs one of our packages, then imports its registry defaults as the machine account"
else fail "package-install sg-office: $r / $(tr '\n' '|' < "$CALLS")"; fi
# ... into the RUNNING shared wineserver: its socket is under /tmp, so the
# service must not have a private /tmp (it had: the import went to a second
# server of its own, and SG Store said "restart, then install it again")
private_tmp() { grep -Eiq '^[[:space:]]*PrivateTmp[[:space:]]*=[[:space:]]*(yes|true|on|1|disconnected)' "$1"; }
private_tmp "$HERE/admin/sg-admind.service" && fail "sg-admind.service has a private /tmp: package-install's registration misses the running wineserver" \
    || pass "sg-admind shares /tmp, so its registration reaches the running shared wineserver"
sed 's/^TimeoutStartSec=/PrivateTmp=yes\nTimeoutStartSec=/' "$HERE/admin/sg-admind.service" > "$T/mut-privtmp.service"
private_tmp "$T/mut-privtmp.service" && pass "MUTANT PRIVATETMP (the old unit) is caught" || fail "PRIVATETMP mutant not detected"
: > "$CALLS"
for p in openssh-server 'sg-office extra' '-o=foo' ''; do
    r=$(ask package-install "$p")
    case "$(first "$r")" in "FAILED "*) ;; *) fail "package-install '$p' was not refused: $r";; esac
done
[ ! -s "$CALLS" ] && pass "refuses any other package, a second word or an option, and apt never runs" || fail "apt ran: $(tr '\n' '|' < "$CALLS")"

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
