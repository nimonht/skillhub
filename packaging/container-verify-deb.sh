#!/bin/sh
# Runs INSIDE a fresh Debian/Ubuntu container: installs the .deb from /pkg the way a user would (recommended packages
# included), then starts the command line and renders the window. Catches missing dependencies, which the build container hides.
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist/x":/pkg:ro ubuntu:24.04 sh /src/packaging/container-verify-deb.sh
set -eu
export LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq python3 git /pkg/skillhub_*.deb >/dev/null
skillhub --version
mkdir -p -m 700 /tmp/xdg
export XDG_RUNTIME_DIR=/tmp/xdg
python3 -I /src/tests/portable_smoke.py "$(command -v skillhub)"
python3 -I /src/tests/gui_launch.py "$(command -v skillhub)" /tmp/shots
dpkg -L skillhub | grep -E "bin/|applications|metainfo" 
