#!/usr/bin/env python3
# store-check.sh's cabinet maker: an uncompressed Microsoft cabinet (the
# format setupapi reads) of the files named, as NAME-IN-CABINET=FILE.
#   store-cab.py OUT.cab 'i386\gatert.dl_=/path/inner.cab' ...
# SPDX-License-Identifier: LGPL-2.1-or-later
import struct, sys

def cab(out, members):
    files = []
    for m in members:
        name, path = m.split("=", 1)
        files.append((name.encode("ascii") + b"\0", open(path, "rb").read()))
    blob = b"".join(data for _, data in files)
    chunks = [blob[i:i + 32768] for i in range(0, len(blob), 32768)] or [b""]
    head, folder = 36, 8
    entries = b""
    off = 0
    for name, data in files:
        # size, offset in the folder, folder 0, a date (2008-04-14), a time, attributes
        entries += struct.pack("<IIHHHH", len(data), off, 0, (28 << 9) | (4 << 5) | 14, 0, 0x20) + name
        off += len(data)
    data_start = head + folder + len(entries)
    blocks = b"".join(struct.pack("<IHH", 0, len(c), len(c)) + c for c in chunks)
    total = data_start + len(blocks)
    header = b"MSCF" + struct.pack("<IIIIIBBHHHHH", 0, total, 0, head + folder, 0, 3, 1, 1, len(files), 0, 0x5347, 0)
    with open(out, "wb") as f:
        f.write(header + struct.pack("<IHH", data_start, len(chunks), 0) + entries + blocks)

cab(sys.argv[1], sys.argv[2:])
