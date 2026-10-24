#!/bin/sh
# Runs INSIDE a Debian/Ubuntu container: builds the .deb from the source tree mounted at /src into /out.
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out debian:trixie sh /src/packaging/container-deb.sh
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential debhelper devscripts equivs fakeroot ca-certificates
rm -rf /build && mkdir /build && cp -a /src/. /build/ && cd /build
rm -rf build .git dist debian
cp -r packaging/debian debian
mk-build-deps -i -r -t "apt-get -y -qq --no-install-recommends" debian/control >/dev/null
dpkg-buildpackage -us -uc -b
mkdir -p /out
cp ../skillhub_*.deb /out/
ls -la /out
