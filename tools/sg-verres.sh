#!/bin/sh
# sg-verres.sh NAME BUILD WINDRES: NAME's version resource (sg-paint64 -> "Paint", from
# src/version-names.txt), compiled to BUILD/NAME-ver.o; prints that path for the link line.
# SPDX-License-Identifier: AGPL-3.0-or-later
n=$1; b=$2; wr=$3
base=${n%64}; base=${base%32}
desc=$(awk -F '\t' -v k="$base" '$1 == k { print $2 }' src/version-names.txt)
[ -n "$desc" ] || { echo "sg-verres: no name for $base in src/version-names.txt" >&2; exit 1; }
v=$(sed -n '1s/.*(\([0-9.]*\)-\([0-9]*\)).*/\1.\2/p' debian/changelog)
[ -n "$v" ] || v=0.0.0.0
vc=$(echo "$v" | tr . ,)
cat > "$b/$n-ver.rc" <<EOT
#include <winver.h>
VS_VERSION_INFO VERSIONINFO
FILEVERSION $vc
PRODUCTVERSION $vc
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "Stained Glass OS"
            VALUE "FileDescription", "$desc"
            VALUE "FileVersion", "$v"
            VALUE "InternalName", "$base"
            VALUE "OriginalFilename", "$n.exe"
            VALUE "ProductName", "Stained Glass OS"
            VALUE "ProductVersion", "$v"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
EOT
"$wr" "$b/$n-ver.rc" -O coff -o "$b/$n-ver.o" || exit 1
echo "$b/$n-ver.o"
