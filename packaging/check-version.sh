#!/bin/sh
# Every file that names the version must agree with CMakeLists.txt: Debian and Fedora name their packages after their own
# files, so a stale one ships new code under an old version.
#   sh packaging/check-version.sh            all files agree
#   sh packaging/check-version.sh 2.0.0      and this tag is the version, with no suffix left (run before publishing)
set -eu
cd "$(dirname "$0")/.."
V=$(sed -n 's/^project(skillhub VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
bad=0
same() {
    [ "$2" = "$V" ] && return
    echo "$1 says '$2', CMakeLists.txt says '$V'" >&2
    bad=1
}
same packaging/debian/changelog "$(sed -n '1s/^skillhub (\([^)-]*\).*/\1/p' packaging/debian/changelog)"
same packaging/fedora/skillhub.spec "$(sed -n 's/^Version:[[:space:]]*//p' packaging/fedora/skillhub.spec)"
same packaging/arch/PKGBUILD "$(sed -n 's/^pkgver=//p' packaging/arch/PKGBUILD)"
same data/io.github.nimonht.skillhub.metainfo.xml "$(sed -n 's/.*<release version="\([^"]*\)".*/\1/p' data/io.github.nimonht.skillhub.metainfo.xml | head -n 1)"
same CHANGELOG.md "$(sed -n 's/^## \([0-9][0-9.]*\).*/\1/p' CHANGELOG.md | head -n 1)"
if [ $# -gt 0 ]; then
    same "the tag" "$1"
    SUFFIX=$(sed -n 's/^set(SKILLHUB_VERSION_SUFFIX "\([^"]*\)".*/\1/p' CMakeLists.txt)
    same "the version with its suffix" "$V$SUFFIX"
fi
exit $bad
