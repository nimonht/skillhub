#!/usr/bin/env python3
"""Builds the portable Linux tarball from a CMake build: the programs with their own Qt, for people who install without root.

usage: python3 packaging/linux/make-tarball.py BUILD VERSION OUT_DIR

Configure the build with -DSKILLHUB_SELF_UPDATE=ON, -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib' and -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON. Needs
Qt's qtpaths on PATH. The last line printed is the tarball's path. What every desktop has (glibc, X11, Wayland, OpenGL, fontconfig) stays
out, so build it on the oldest system it should run on.
"""
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile

BUILD, VERSION, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHIPPED = re.compile(r"^(libQt6|libicu|libxcb-cursor)")
PLUGINS = ["platforms", "platforminputcontexts", "xcbglintegrations", "wayland-decoration-client", "wayland-graphics-integration-client",
           "wayland-shell-integration", "imageformats", "iconengines", "tls"]


def fail(msg):
    sys.exit("make-tarball: " + msg)


def qt(name):
    qtpaths = shutil.which("qtpaths6") or shutil.which("qtpaths") or fail("qtpaths was not found")
    return subprocess.run([qtpaths, "--query", name], check=True, capture_output=True, text=True).stdout.strip()


def is_elf(path):
    with open(path, "rb") as f:
        return f.read(4) == b"\x7fELF"


name = "skillhub-" + VERSION
stage = os.path.join(OUT, name)
shutil.rmtree(stage, ignore_errors=True)
for folder in ("bin", "lib"):
    os.makedirs(os.path.join(stage, folder))
for program in ("skillhub", "skillhub-gui"):
    shutil.copy2(os.path.join(BUILD, program), os.path.join(stage, "bin"))
with open(os.path.join(stage, "bin", "qt.conf"), "w") as f:
    f.write("[Paths]\nPrefix = ..\nPlugins = plugins\nQmlImports = qml\n")
dynamic = subprocess.run(["readelf", "-d", os.path.join(stage, "bin", "skillhub-gui")], check=True, capture_output=True, text=True).stdout
if "$ORIGIN/../lib" not in dynamic:
    fail("skillhub-gui does not look in ../lib: see the usage above")

for plugin in PLUGINS:
    source = os.path.join(qt("QT_INSTALL_PLUGINS"), plugin)
    if os.path.isdir(source):
        shutil.copytree(source, os.path.join(stage, "plugins", plugin), symlinks=True)

# the QML modules the window imports, and what those import
qml_root = qt("QT_INSTALL_QML")
scanner = os.path.join(qt("QT_HOST_LIBEXECS"), "qmlimportscanner")
found = subprocess.run([scanner, "-rootPath", os.path.join(ROOT, "qml"), "-importPath", qml_root], check=True, capture_output=True, text=True).stdout
modules = 0
for entry in json.loads(found):
    path = entry.get("path", "")
    if not path or not path.startswith(qml_root):
        continue  # the window's own files, which are compiled in
    target = os.path.join(stage, "qml", os.path.relpath(path, qml_root))
    if not os.path.isdir(target):
        shutil.copytree(path, target, symlinks=True, ignore=shutil.ignore_patterns("*.debug", "*.qmlc"))
        modules += 1
if not modules:
    fail("no QML module was found")

# every library those need, until nothing is missing
queue = [os.path.join(stage, "bin", "skillhub-gui")]
for folder in ("plugins", "qml"):
    for parent, _, names in os.walk(os.path.join(stage, folder)):
        queue += [os.path.join(parent, n) for n in names if n.endswith(".so") or ".so." in n]
seen = set()
missing = set()
while queue:
    path = queue.pop()
    if path in seen or os.path.islink(path) or not is_elf(path):
        continue
    seen.add(path)
    for line in subprocess.run(["ldd", path], capture_output=True, text=True).stdout.splitlines():
        m = re.match(r"\s*(\S+) => (\S+)", line)
        if not m or not SHIPPED.match(m.group(1)):
            continue
        if m.group(2) == "not":
            missing.add(m.group(1))
            continue
        target = os.path.join(stage, "lib", m.group(1))
        if not os.path.exists(target):
            shutil.copy2(os.path.realpath(m.group(2)), target)
            os.chmod(target, 0o755)
            queue.append(target)
if missing:
    fail("not found: %s" % ", ".join(sorted(missing)))
if not os.path.exists(os.path.join(stage, "lib", "libxcb-cursor.so.0")):
    fail("libxcb-cursor is missing, and the X11 plugin needs it: install libxcb-cursor0 on the build machine")

for file in ("LICENSE", "NOTICE.md", "README.md"):
    shutil.copy2(os.path.join(ROOT, file), stage)
shutil.copy2(os.path.join(ROOT, "packaging", "linux", "install.sh"), stage)
shutil.copytree(os.path.join(ROOT, "data", "icons", "hicolor"), os.path.join(stage, "share", "icons", "hicolor"))

machine = platform.machine().lower()
tarball = os.path.join(OUT, "%s-linux-%s.tar.gz" % (name, "aarch64" if machine in ("aarch64", "arm64") else "x86_64"))
with tarfile.open(tarball, "w:gz", compresslevel=9) as t:
    t.add(stage, arcname=name)
print("%d QML modules, %d libraries, %d MB" % (modules, len(os.listdir(os.path.join(stage, "lib"))), os.path.getsize(tarball) // 1000000))
print(tarball)
