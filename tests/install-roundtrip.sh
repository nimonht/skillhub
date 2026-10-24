#!/bin/sh
# `make install` then `make uninstall` into a fake root must remove only our files, never shared directories.
# usage: tests/install-roundtrip.sh   (run from the repository root; uses ./build-roundtrip and cleans up)
set -eu
SRC=$(pwd)
B=$(mktemp -d); R=$(mktemp -d)
trap 'rm -rf "$B" "$R"' EXIT
mkdir -p "$R/usr/share/icons/hicolor/48x48/apps" "$R/usr/share/applications"
touch "$R/usr/share/icons/hicolor/48x48/apps/other.png" "$R/usr/share/applications/other.desktop"
cd "$B"
${QMAKE:-$(command -v qmake6 || command -v qmake)} PREFIX=/usr "$SRC/skillhub.pro" >/dev/null
make -s -j"$(nproc 2>/dev/null || echo 2)" >/dev/null
make -s install INSTALL_ROOT="$R" >/dev/null
[ -x "$R/usr/bin/skillhub" ] && [ -f "$R/usr/share/icons/hicolor/48x48/apps/skillhub.png" ] || { echo "FAIL: install incomplete" >&2; exit 1; }
make -s uninstall INSTALL_ROOT="$R" >/dev/null 2>&1   # rmdir noise = non-empty shared dirs, as intended
[ ! -e "$R/usr/bin/skillhub" ] && [ ! -e "$R/usr/share/applications/skillhub.desktop" ] && [ ! -e "$R/usr/share/metainfo/io.github.nimonht.skillhub.metainfo.xml" ] && [ ! -e "$R/usr/share/icons/hicolor/48x48/apps/skillhub.png" ] || { echo "FAIL: uninstall left our files" >&2; exit 1; }
[ -f "$R/usr/share/icons/hicolor/48x48/apps/other.png" ] && [ -f "$R/usr/share/applications/other.desktop" ] || { echo "FAIL: uninstall removed foreign files" >&2; exit 1; }
echo "install round-trip passed"
