#!/bin/sh
# `cmake --install` then the `uninstall` target into a fake root must remove only our files, never shared directories.
# usage: tests/install-roundtrip.sh   (run from the repository root; builds in a temporary directory; CMAKE=... to override)
set -eu
SRC=$(pwd)
CMAKE=${CMAKE:-cmake}
B=$(mktemp -d); R=$(mktemp -d)
trap 'rm -rf "$B" "$R"' EXIT
mkdir -p "$R/usr/share/icons/hicolor/48x48/apps" "$R/usr/share/applications"
touch "$R/usr/share/icons/hicolor/48x48/apps/other.png" "$R/usr/share/applications/other.desktop"
"$CMAKE" -S "$SRC" -B "$B" -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
"$CMAKE" --build "$B" -j"$(nproc 2>/dev/null || echo 2)" >/dev/null
DESTDIR="$R" "$CMAKE" --install "$B" >/dev/null
for f in usr/bin/skillhub usr/bin/skillhub-gui; do
  [ -x "$R/$f" ] || { echo "FAIL: install incomplete: $f" >&2; exit 1; }
done
[ -f "$R/usr/share/icons/hicolor/48x48/apps/skillhub.png" ] && [ -f "$R/usr/share/applications/skillhub.desktop" ] || { echo "FAIL: install incomplete" >&2; exit 1; }
DESTDIR="$R" "$CMAKE" --build "$B" --target uninstall >/dev/null
for f in usr/bin/skillhub usr/bin/skillhub-gui usr/share/applications/skillhub.desktop \
         usr/share/metainfo/io.github.nimonht.skillhub.metainfo.xml usr/share/icons/hicolor/48x48/apps/skillhub.png \
         usr/share/icons/hicolor/scalable/apps/skillhub.svg; do
  [ ! -e "$R/$f" ] || { echo "FAIL: uninstall left $f" >&2; exit 1; }
done
[ -f "$R/usr/share/icons/hicolor/48x48/apps/other.png" ] && [ -f "$R/usr/share/applications/other.desktop" ] || { echo "FAIL: uninstall removed foreign files" >&2; exit 1; }
[ -d "$R/usr/share/icons/hicolor/48x48/apps" ] || { echo "FAIL: uninstall removed a shared directory" >&2; exit 1; }
echo "install round-trip passed"
