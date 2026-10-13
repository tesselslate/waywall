#!/usr/bin/env bash
# archlinux:base-devel

# setup
set -e
mkdir -p /out
rm -rf /out/*

# install deps
pacman -Syu --noconfirm \
    cmake \
    git \
    wayland \
    libxkbcommon \
    libx11 \
    libxrandr \
    libxinerama \
    libxcursor \
    libxi

# checkout glfw
git clone https://github.com/glfw/glfw
cd glfw
git checkout 3.4
git apply ../contrib/glfw.patch

# build glfw
cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DGLFW_STANDALONE=ON -DGLFW_BUILD_WAYLAND=ON
cd build
make glfw

# copy outputs
cd ..
cp build/src/libglfw.so /out/libglfw.so.3.4
ln -s libglfw.so.3.4    /out/libglfw.so.3
ln -s libglfw.so.3      /out/libglfw.so
cp LICENSE.md           /out/LICENSE.md
cp CONTRIBUTORS.md      /out/CONTRIBUTORS.md

cat > "/out/PATCHES.md" << 'EOF'
GLFW Maintainers and Contributors:
- See https://github.com/glfw/glfw/blob/3.4/CONTRIBUTORS.md for the list of contributors (or the CONTRIBUTORS.md file).

Note:
This version of GLFW (3.4) includes a patch from the waywall project (https://github.com/tesselslate/waywall)
to enable compatibility with Minecraft when used with waywall. The patch is applied from:
https://raw.githubusercontent.com/tesselslate/waywall/be3e018bb5f7c25610da73cc320233a26dfce948/contrib/glfw.patch
EOF
