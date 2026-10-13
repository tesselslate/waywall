#!/usr/bin/env bash
# debian:13

# setup
set -e
mkdir -p /out
rm -rf /out/*

# meson setup --wipe is insufficient and the build directory must be deleted
# entirely, since --wipe preserves build configuration from the host
rm -rf build

cp -r contrib/debian debian

# install deps
apt-get update
apt-get install -y build-essential clang
apt-get build-dep -y .

# build package
#
# clang is required due to the packaged version of GCC being outdated and not
# supporting some features waywall requires
#
# for -gdwarf-4: see https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=1016936
CC=clang CFLAGS=-gdwarf-4 dpkg-buildpackage -b -us -uc

# copy outputs
for f in ../*.deb; do
    cp "$f" /out
done
