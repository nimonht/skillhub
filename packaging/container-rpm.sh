#!/bin/sh
# Runs INSIDE a Fedora container: builds the .rpm from the source tree mounted at /src into /out.
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out fedora:latest sh /src/packaging/container-rpm.sh
set -eu
dnf -y -q install rpm-build rpmdevtools dnf-plugins-core tar gzip >/dev/null
V=$(sed -n 's/^Version:[[:space:]]*//p' /src/packaging/fedora/skillhub.spec)
rm -rf /build /root/rpmbuild && mkdir -p /build/skillhub-$V /root/rpmbuild/SOURCES
cp -a /src/. /build/skillhub-$V/
rm -rf /build/skillhub-$V/build /build/skillhub-$V/.git /build/skillhub-$V/dist
tar -C /build -czf /root/rpmbuild/SOURCES/skillhub-$V.tar.gz skillhub-$V
cp /src/packaging/fedora/skillhub.spec /root/rpmbuild/
dnf -y -q builddep /root/rpmbuild/skillhub.spec >/dev/null
rpmbuild -ba /root/rpmbuild/skillhub.spec
mkdir -p /out
find /root/rpmbuild/RPMS -name "skillhub-*.rpm" ! -name "*debuginfo*" ! -name "*debugsource*" -exec cp {} /out/ \;
ls -la /out
