#!/usr/bin/env python3
"""Switch a Windows executable between the "window" and the "console" subsystem, in place.

  python3 tools/exe-subsystem.py skillhub-cli.exe console

Skill Hub is one binary: the window build opens no console when started from Explorer, so a copy marked "console" is shipped
as skillhub-cli.exe for terminals and scripts (a window-subsystem program cannot print to a terminal reliably).
"""
import struct
import sys

SUBSYSTEMS = {"window": 2, "console": 3}

if len(sys.argv) != 3 or sys.argv[2] not in SUBSYSTEMS:
    sys.exit(__doc__)
path = sys.argv[1]
with open(path, "r+b") as f:
    data = bytearray(f.read())
    if data[:2] != b"MZ":
        sys.exit("not an executable: " + path)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        sys.exit("no PE header in " + path)
    offset = pe + 24 + 68  # optional header + Subsystem (same offset for PE32 and PE32+)
    struct.pack_into("<H", data, offset, SUBSYSTEMS[sys.argv[2]])
    f.seek(0)
    f.write(data)
print("%s: %s subsystem" % (path, sys.argv[2]))
