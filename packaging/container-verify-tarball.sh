#!/bin/sh
# Runs INSIDE a fresh container: the portable tarball must install and start on a system that never saw Qt, as a normal user.
#   docker run --rm -v "$PWD":/src:ro fedora:43 sh /src/packaging/container-verify-tarball.sh /src/dist/skillhub-VERSION-linux-x86_64.tar.gz
set -eu
tarball=$1
if command -v apt-get >/dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends python3 git ca-certificates libgl1 libegl1 libfontconfig1 libdbus-1-3 libglib2.0-0t64 libxkbcommon0 libx11-6 >/dev/null
elif command -v dnf >/dev/null; then
    dnf install -y -q python3 git-core mesa-libGL mesa-libEGL fontconfig dbus-libs glib2 libxkbcommon libX11 shadow-utils
elif command -v pacman >/dev/null; then
    # -u: Arch supports no partial upgrade. A new package beside an image that is a few days old once claimed a file the old one still owned.
    pacman -Syu --noconfirm --needed python git libglvnd fontconfig dbus glib2 libxkbcommon libx11 >/dev/null
fi
if ldconfig -p | grep -q libQt6Core; then
    echo "this system has Qt, so the test proves nothing" >&2
    exit 1
fi
useradd -m tester
su tester -s /bin/sh -c "
    set -eu
    cd \$HOME
    tar xzf $tarball
    sh skillhub-*/install.sh
    export PATH=\$HOME/.local/bin:\$PATH
    skillhub --version
    readlink \$HOME/.local/share/skillhub/app/current
    python3 -I /src/tests/gui_launch.py \$(command -v skillhub) /tmp/shots
    skillhub uninstall --yes
    test ! -e \$HOME/.local/share/skillhub/app
    test ! -e \$HOME/.local/bin/skillhub
"
echo "the portable tarball works here"
