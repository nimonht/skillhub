#!/bin/sh
# The files installed copies ask a release for, by the names they were shipped with (asset_for in src/core/sh_selfupdate.c). A release
# that lacks one, or names it differently, leaves every copy of that kind where it is, and no later release can repair them.
#   sh packaging/check-release-files.sh 2.0.0 artifacts/SHA256SUMS
set -eu
V=$1
SUMS=$2
bad=0
for name in "SkillHub-$V-windows-x64-setup.exe" "SkillHub-$V-macos-universal.dmg" "skillhub-$V-linux-x86_64.tar.gz" "skillhub-$V-linux-aarch64.tar.gz"; do
    if ! grep -Eq "^[0-9a-f]{64} [ *]$name\$" "$SUMS"; then
        echo "$SUMS does not list $name, which installed copies update themselves from" >&2
        bad=1
    fi
done
exit $bad
