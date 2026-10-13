#!/usr/bin/env bash
# archlinux:base-devel

# setup
set -e
mkdir -p /out
rm -rf /out/*

# meson setup --wipe is insufficient and the build directory must be deleted
# entirely, since --wipe preserves build configuration from the host
rm -rf build

# install deps (all package deps must be installed due to 'nobody' lacking root)
pacman -Syu --noconfirm \
    meson \
    git \
    wayland-protocols \
    libegl \
    libgles \
    luajit \
    libspng \
    wayland \
    libxcb \
    libxkbcommon \
    xorg-xwayland

# build package
chown -R nobody:nobody "$PWD"
WAYWALL_SRC="$PWD"
cd contrib
runuser -u nobody -- sh -c "makepkg -sf --noconfirm WAYWALL_SRC=\"$WAYWALL_SRC\""

# copy outputs
for f in *.tar.zst; do
    cp "$f" /out
done
