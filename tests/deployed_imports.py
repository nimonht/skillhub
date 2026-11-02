#!/usr/bin/env python3
"""A deployed Windows folder stands on its own: every Qt or Visual C++ DLL that a program in it imports is in the folder too.

usage: python3 tests/deployed_imports.py DIST/SkillHub

Starting the program on a build machine proves little: Qt is on PATH there and Visual Studio brought the debug C runtime. This reads
the import tables instead, so it needs neither Windows nor a toolchain. A Debug build fails here because its debug DLLs are not deployed.
"""
import os
import re
import struct
import sys

# what Windows brings is no concern here; these come with the program, or (ucrtbased) can never be shipped
OURS = re.compile(r"^(qt\d|vcruntime|msvcp|concrt|vccorlib|ucrtbased)", re.IGNORECASE)
IMPORT_DIRECTORY = 1


def imports(path):
    """The DLL names in the import table of the PE file at `path`."""
    with open(path, "rb") as f:
        data = f.read()

    def string_at(offset):
        return data[offset:data.index(b"\0", offset)].decode("ascii")

    if data[:2] != b"MZ":
        raise ValueError("not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    sections, _, _, _, optional_size = struct.unpack_from("<HIIIH", data, pe + 6)
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories = optional + {0x10B: 96, 0x20B: 112}[magic]
    table = struct.unpack_from("<I", data, directories + IMPORT_DIRECTORY * 8)[0]
    if not table:
        return []
    spans = []
    for i in range(sections):
        header = optional + optional_size + i * 40
        virtual_size, virtual_address, raw_size, raw_address = struct.unpack_from("<IIII", data, header + 8)
        spans.append((virtual_address, max(virtual_size, raw_size), raw_address))

    def offset_of(rva):
        for start, size, raw in spans:
            if start <= rva < start + size:
                return raw + rva - start
        raise ValueError("address %#x is in no section" % rva)

    names = []
    entry = offset_of(table)
    while True:
        descriptor = struct.unpack_from("<IIIII", data, entry)
        if not any(descriptor):
            return names
        names.append(string_at(offset_of(descriptor[3])))
        entry += 20


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        sys.exit(__doc__)
    root = sys.argv[1]
    here = {name.lower() for name in os.listdir(root)}
    missing = {}
    programs = found = 0
    for folder, _, files in os.walk(root):
        for name in files:
            if not name.lower().endswith((".exe", ".dll")):
                continue
            programs += 1
            path = os.path.join(folder, name)
            try:
                dlls = imports(path)
            except (ValueError, KeyError, struct.error) as e:
                sys.exit("FAIL: %s: %s" % (path, e))
            for dll in dlls:
                if OURS.match(dll):
                    found += 1
                    if dll.lower() not in here or dll.lower() == "ucrtbased.dll":
                        missing.setdefault(dll, []).append(os.path.relpath(path, root))
    if not programs:
        sys.exit("FAIL: no programs in %s" % root)
    if missing:
        print("FAIL: imported, but not in the folder (a Debug build, or a DLL that was not deployed):", file=sys.stderr)
        for dll in sorted(missing, key=str.lower):
            print("  %-32s needed by %s" % (dll, ", ".join(sorted(missing[dll])[:4])), file=sys.stderr)
        sys.exit(1)
    print("ok  %d programs and libraries, %d imports of Qt and the C++ runtime, all in the folder" % (programs, found))


main()
