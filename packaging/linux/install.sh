#!/bin/sh
# Installs this portable Skill Hub for you alone, below ~/.local, without root. It updates itself from then on (skillhub self-update, or
# the button in the window), and `skillhub uninstall` removes it.
#   tar xf skillhub-*-linux-*.tar.gz && sh skillhub-*/install.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
version=$("$here/bin/skillhub" --version | sed 's/^skillhub //')
root=$HOME/.local/share/skillhub/app
bin=$HOME/.local/bin

mkdir -p "$root" "$bin"
if [ "$here" != "$root/$version" ]; then
    rm -rf "${root:?}/${version:?}"
    cp -a "$here" "$root/$version"
fi
# a link to the version in use: updating is one rename, and everything else keeps naming the same path
ln -sfn "$version" "$root/current"
ln -sf "$root/current/bin/skillhub" "$bin/skillhub"
ln -sf "$root/current/bin/skillhub-gui" "$bin/skillhub-gui"

share=$HOME/.local/share
mkdir -p "$share/applications"
cp -R "$here/share/icons" "$share/"
cat > "$share/applications/skillhub.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Skill Hub
GenericName=Coding Agent Skill Manager
Comment=One skills folder for every coding agent
Exec=$root/current/bin/skillhub gui
Icon=skillhub
Terminal=false
Categories=Development;
StartupWMClass=skillhub
DESKTOP
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$share/applications" >/dev/null 2>&1 || true

echo "Skill Hub $version is installed in $root"
# what the shell finds, not how PATH spells it (~/.local/share/../bin is ~/.local/bin too)
if [ "$(command -v skillhub 2>/dev/null)" -ef "$bin/skillhub" ]; then
    echo "Run: skillhub"
else
    echo "$bin is not on your PATH yet. Add it, or run: $bin/skillhub"
fi
