#!/bin/sh
# version-check: every program we ship says what it is called. Open with, Task
# Manager and a program's Properties read the version resource's FileDescription
# (Windows' own programs all carry one); without it Open with listed Paint as
# "Sg-paint64". tools/sg-verres.sh builds it from src/version-names.txt.
# SPDX-License-Identifier: AGPL-3.0-or-later
cd "$(dirname "$0")/.." || exit 1
B=${BUILD:-build}
ls "$B"/sg-*.exe >/dev/null 2>&1 || { echo "SKIP: nothing built"; exit 77; }
fail=0; n=0
for f in "$B"/sg-*.exe; do
    case $f in *-mut.exe|*-[A-Z]*.exe) continue ;; esac
    n=$((n + 1))
    python3 -c "import sys; d = open(sys.argv[1], 'rb').read(); sys.exit(0 if 'FileDescription'.encode('utf-16-le') in d else 1)" "$f" \
        || { echo "FAIL: $f has no FileDescription (add it to src/version-names.txt)"; fail=1; }
done
# the name is the one Start shows
python3 -c "import sys; d = open(sys.argv[1], 'rb').read(); sys.exit(0 if '\0Paint\0'.encode('utf-16-le') in d else 1)" "$B/sg-paint64.exe" \
    || { echo "FAIL: sg-paint64.exe is not called Paint"; fail=1; }
python3 -c "import sys; d = open(sys.argv[1], 'rb').read(); sys.exit(0 if 'Alarms & Clock'.encode('utf-16-le') in d else 1)" "$B/sg-clock64.exe" \
    || { echo "FAIL: sg-clock64.exe is not called Alarms & Clock"; fail=1; }
[ $fail = 0 ] && echo "PASS: $n programs carry their names"
exit $fail
