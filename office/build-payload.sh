#!/bin/sh
# SG Office -- the payload Get SG Office puts on top of LibreOffice, built
# into DIR: office.ini, sg-office.xcd.in, templates/, extensions/sg-office-functions/.
#
#   build-payload.sh DIR
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
OUT=$1
rm -rf "$OUT"
mkdir -p "$OUT/extensions"
cp "$HERE/office.ini" "$HERE/registry/sg-office.xcd.in" "$HERE/registry/sg-office-user.xcu" "$OUT/"
python3 "$HERE/templates/gen-templates.py" "$OUT/templates"
python3 "$HERE/functions/build-oxt.py" "$OUT/extensions/sg-office-functions"
# the spreadsheet ribbon, its Home tab as Excel's (from LibreOffice's own, for
# the version office.ini pins)
V=$(sed -n 's/^Version=//p' "$HERE/office.ini")
mkdir -p "$OUT/ui/scalc"
python3 "$HERE/notebookbar/gen-calc.py" "$HERE/notebookbar/scalc-notebookbar-$V.ui" "$OUT/ui/scalc/notebookbar_sgoffice.ui"
