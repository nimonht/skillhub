#!/bin/sh
# Runs INSIDE a fresh Fedora container: installs the .rpm from /pkg, starts the command line and renders the window.
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist/x":/pkg:ro fedora:43 sh /src/packaging/container-verify-rpm.sh
set -eu
export LC_ALL=C.UTF-8
dnf -y -q install python3 git-core /pkg/skillhub-*.rpm >/dev/null
skillhub --version
test -x "$(dirname "$(command -v skillhub)")/skillhub-gui"   # the window sits next to the command line
mkdir -p -m 700 /tmp/xdg
export XDG_RUNTIME_DIR=/tmp/xdg
python3 -I /src/tests/portable_smoke.py "$(command -v skillhub)"
python3 -I /src/tests/gui_launch.py "$(command -v skillhub)" /tmp/shots
rpm -ql skillhub | grep -E "bin/|applications|metainfo"
