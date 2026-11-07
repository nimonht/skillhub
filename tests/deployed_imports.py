#!/usr/bin/env python3
"""A deployed Windows folder stands on its own: every Qt or Visual C++ DLL that a program in it imports is in the folder too.

usage: python3 tests/deployed_imports.py DIST/SkillHub
       python3 tests/deployed_imports.py --forbidden-only BUILD    only the promise below, for a build folder that has no Qt next to it

Starting the program on a build machine proves little: Qt is on PATH there and Visual Studio brought the debug C runtime. This reads
the import tables instead, so it needs neither Windows nor a toolchain. A Debug build fails here because its debug DLLs are not deployed.

It also holds Skill Hub's own programs (skillhub*.exe) to what they promise: they never read or write another process's memory and
never write a memory dump. An unsigned program that imports those functions looks like a credential stealer to an antivirus, and
Skill Hub has no use for them (the memory dump of docs is made with the system's own Task Manager, SECURITY.md). A new import of
one is a decision to make by hand, not by accident: take it out of FORBIDDEN here only with that decision written down.
"""
import os
import re
import struct
import sys

# what Windows brings is no concern here; these come with the program, or (ucrtbased) can never be shipped
OURS = re.compile(r"^(qt\d|vcruntime|msvcp|concrt|vccorlib|ucrtbased)", re.IGNORECASE)
IMPORT_DIRECTORY = 1
DELAY_IMPORT_DIRECTORY = 13
FORBIDDEN_DLLS = {"dbghelp.dll"}
FORBIDDEN = {"minidumpwritedump", "readprocessmemory", "writeprocessmemory", "virtualallocex", "virtualprotectex", "createremotethread",
             "createremotethreadex", "ntreadvirtualmemory", "ntwritevirtualmemory", "setwindowshookexa", "setwindowshookexw"}


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


def import_functions(path):
    """{dll name (lower case): [function names]} of the import table and the delay-load table of the PE file at `path`."""
    with open(path, "rb") as f:
        data = f.read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections, _, _, _, optional_size = struct.unpack_from("<HIIIH", data, pe + 6)
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    wide = magic == 0x20B
    directories = optional + {0x10B: 96, 0x20B: 112}[magic]
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

    def string_at(rva):
        offset = offset_of(rva)
        return data[offset:data.index(b"\0", offset)].decode("ascii")

    def functions(thunks):
        names, size = [], 8 if wide else 4
        offset = offset_of(thunks)
        while True:
            entry = struct.unpack_from("<Q" if wide else "<I", data, offset)[0]
            if not entry:
                return names
            if not entry >> (63 if wide else 31):   # by name, not by number
                names.append(string_at((entry & 0x7FFFFFFF) + 2))
            offset += size

    found = {}
    table = struct.unpack_from("<I", data, directories + IMPORT_DIRECTORY * 8)[0]
    entry = offset_of(table) if table else 0
    while entry:
        original, _, _, name, first = struct.unpack_from("<IIIII", data, entry)
        if not (original or name or first):
            break
        found.setdefault(string_at(name).lower(), []).extend(functions(original or first))
        entry += 20
    delay = struct.unpack_from("<I", data, directories + DELAY_IMPORT_DIRECTORY * 8)[0]
    entry = offset_of(delay) if delay else 0
    while entry:
        attributes, name, _, _, names = struct.unpack_from("<IIIII", data, entry)
        if not (attributes or name):
            break
        if attributes & 1:   # addresses are relative; the old form with absolute ones is not made by any current linker
            found.setdefault(string_at(name).lower(), []).extend(functions(names))
        entry += 32
    return found


def forbidden_imports(root):
    """What skillhub*.exe in `root` imports that it promises never to."""
    bad = []
    for name in sorted(os.listdir(root)):
        if not (name.lower().startswith("skillhub") and name.lower().endswith(".exe")):
            continue
        for dll, names in import_functions(os.path.join(root, name)).items():
            if dll in FORBIDDEN_DLLS:
                bad.append("%s imports %s" % (name, dll))
            bad += ["%s imports %s from %s" % (name, fn, dll) for fn in names if fn.lower() in FORBIDDEN]
    return bad


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--forbidden-only" and os.path.isdir(sys.argv[2]):
        bad = forbidden_imports(sys.argv[2])
        for line in bad:
            print("FAIL: " + line, file=sys.stderr)
        if not bad:
            print("ok  no program of ours reads another's memory or writes a dump")
        sys.exit(1 if bad else 0)
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
    bad = forbidden_imports(root)
    if bad:
        print("FAIL: Skill Hub's programs import what they promise never to (see this file's docstring):", file=sys.stderr)
        for line in bad:
            print("  " + line, file=sys.stderr)
        sys.exit(1)
    if missing:
        print("FAIL: imported, but not in the folder (a Debug build, or a DLL that was not deployed):", file=sys.stderr)
        for dll in sorted(missing, key=str.lower):
            print("  %-32s needed by %s" % (dll, ", ".join(sorted(missing[dll])[:4])), file=sys.stderr)
        sys.exit(1)
    print("ok  %d programs and libraries, %d imports of Qt and the C++ runtime, all in the folder; no program of ours reads another's memory or writes a dump" % (programs, found))


main()
