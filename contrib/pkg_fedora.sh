#!/usr/bin/env bash
# fedora:43

# setup
set -e
mkdir -p /out
rm -rf /out/*

# meson setup --wipe is insufficient and the build directory must be deleted
# entirely, since --wipe preserves build configuration from the host
rm -rf build

# install deps
dnf -y install rpmbuild
dnf -y builddep contrib/waywall.spec

# build package
# see https://github.com/rpm-software-management/rpm/issues/4037
rpmbuild -ba \
    --build-in-place \
    --define "_srcrpmdir /out" \
    --define "_rpmdir /out" \
    --undefine "_debugsource_packages" \
    contrib/waywall.spec

# copy outputs
for f in /out/x86_64/*; do
    cp "$f" /out
done
