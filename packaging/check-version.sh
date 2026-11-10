#!/bin/sh
# Every file that names the version must agree with CMakeLists.txt: Debian and Fedora name their packages after their own
# files, so a stale one ships new code under an old version.
#   sh packaging/check-version.sh                  all files agree
#   sh packaging/check-version.sh 2.0.0-beta1      and this tag is the version, and is fit to publish (run before publishing)
# A pre-release is written three ways: 2.0.0-beta1 for people, 2.0.0~beta1 where a tilde makes it sort before 2.0.0 (Debian, Fedora,
# AppStream), and 2.0.0beta1 in the PKGBUILD, where a hyphen is forbidden. The -dev suffix between releases appears in no package file.
set -eu
cd "$(dirname "$0")/.."
V=$(sed -n 's/^project(skillhub VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
SUFFIX=$(sed -n 's/^set(SKILLHUB_VERSION_SUFFIX "\([^"]*\)".*/\1/p' CMakeLists.txt)
PRE=${SUFFIX#-}
[ "$PRE" = dev ] && PRE=
bad=0
same() {
    [ "$2" = "$3" ] && return
    echo "$1 says '$2', CMakeLists.txt says '$3'" >&2
    bad=1
}
same packaging/debian/changelog "$(sed -n '1s/^skillhub (\([^)-]*\).*/\1/p' packaging/debian/changelog)" "$V${PRE:+~$PRE}"
same packaging/fedora/skillhub.spec "$(sed -n 's/^Version:[[:space:]]*//p' packaging/fedora/skillhub.spec)" "$V${PRE:+~$PRE}"
same packaging/arch/PKGBUILD "$(sed -n 's/^pkgver=//p' packaging/arch/PKGBUILD)" "$V$PRE"
same data/io.github.nimonht.skillhub.metainfo.xml "$(sed -n 's/.*<release version="\([^"]*\)".*/\1/p' data/io.github.nimonht.skillhub.metainfo.xml | head -n 1)" "$V${PRE:+~$PRE}"
same CHANGELOG.md "$(sed -n 's/^## \([0-9][0-9A-Za-z.-]*\) .*/\1/p' CHANGELOG.md | head -n 1)" "$V${PRE:+-$PRE}"
if [ $# -gt 0 ]; then
    same "the tag" "$1" "$V$SUFFIX"
    # what every installed copy can read as a version (sh_selfupdate.c): 2.1.0, or 2.1.0-beta2 with lowercase letters and an optional number
    if ! printf '%s\n' "$1" | grep -Eq '^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}(-[a-z]{1,15}[0-9]{0,9})?$'; then
        echo "'$1' is not a version installed copies can read, so none of them would be offered it (2.1.0, 2.1.0-beta2 and 2.1.0-rc1 are)" >&2
        bad=1
    fi
    if ! grep -Eq '^[0-9a-f]{64}$' data/update-key.pub; then
        echo "data/update-key.pub holds no key, so this release could not update itself: run tools/update-key.sh once" >&2
        bad=1
    fi
    if [ "$SUFFIX" = -dev ]; then
        echo "the version still ends in -dev" >&2
        bad=1
    fi
fi
exit $bad
