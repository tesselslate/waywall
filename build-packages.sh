#!/bin/bash

set -e  # Exit on errors

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WAYWALL_DIR="$SCRIPT_DIR"
OUTPUT_DIR="${WAYWALL_OUTPUT_DIR:-$(realpath "$WAYWALL_DIR/../waywall-build")}"
if [ -z "$1" ]; then
  echo "No output directory specified. Using default: $OUTPUT_DIR"
else
  OUTPUT_DIR="$(realpath "$1")"
fi

mkdir -p "$OUTPUT_DIR"

DISTROS="archlinux fedora debian"
IMAGE_TAGS="archlinux:archlinux fedora:fedora-42 debian:debian-trixie"

# Check if GLFW files already exist
GLFW_SKIP=true
for file in "$WAYWALL_DIR/glfw/MAINTAINERS.md" "$WAYWALL_DIR/glfw/CONTRIBUTORS.md" "$WAYWALL_DIR/glfw/LICENSE.md" "$WAYWALL_DIR/glfw/libglfw.so" "$WAYWALL_DIR/glfw/libglfw.so.3" "$WAYWALL_DIR/glfw/libglfw.so.3.4"; do
  if [ ! -f "$file" ]; then
    GLFW_SKIP=false
    break
  fi
done

if $GLFW_SKIP; then
  echo "GLFW files already exist in $WAYWALL_DIR/glfw/"
  echo "Skipping GLFW build and copy."
  echo "If you want to rebuild GLFW, please remove the existing files in $WAYWALL_DIR/glfw/."
else
  # Step 1: Build the patched GLFW and prepare license/contributors/maintainers
  echo "Building patched GLFW for Minecraft..."

  # Clean up GLFW build directory (in case an error occurred previously)
  rm -rf "$OUTPUT_DIR/glfw-build"

  # Create a podman container for building GLFW
  mkdir -p "$OUTPUT_DIR/glfw-build"  # Create the host directory for mounting
  podman run --rm --pull=never -v "$OUTPUT_DIR/glfw-build:/glfw-build:Z" --workdir /glfw-build --entrypoint /bin/bash localhost/pacur/archlinux -c "
      pacman -Syu --noconfirm cmake ninja wayland libxkbcommon libx11 libxrandr libxinerama libxcursor libxi
      git clone https://github.com/glfw/glfw .
      git checkout 3.4
      curl -o glfw.patch https://raw.githubusercontent.com/tesselslate/waywall/be3e018bb5f7c25610da73cc320233a26dfce948/contrib/glfw.patch
      git apply glfw.patch
      cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DGLFW_BUILD_WAYLAND=ON
      cd build
      make
    "
  GLFW_LIB_DIR="$OUTPUT_DIR/glfw-build/build/src"
  GLFW_FILES=("$GLFW_LIB_DIR/libglfw.so" "$GLFW_LIB_DIR/libglfw.so.3" "$GLFW_LIB_DIR/libglfw.so.3.4")
  for file in "${GLFW_FILES[@]}"; do
    if [ ! -f "$file" ]; then
      echo "Error: Failed to build $file"
      exit 1
    fi
  done

  # Copy the zlib license and contributors
  GLFW_LICENSE="$OUTPUT_DIR/glfw-build/LICENSE.md"
  GLFW_CONTRIBUTORS="$OUTPUT_DIR/glfw-build/CONTRIBUTORS.md"
  for file in "$GLFW_LICENSE" "$GLFW_CONTRIBUTORS"; do
    if [ ! -f "$file" ]; then
      echo "Error: $file not found"
      exit 1
    fi
  done

  # Create MAINTAINERS.md
  cat > "$OUTPUT_DIR/glfw-build/MAINTAINERS.md" << 'EOF'
GLFW Maintainers and Contributors:
- See https://github.com/glfw/glfw/blob/3.4/CONTRIBUTORS.md for the list of contributors (or the bundled Contributors.md file).

Note:
This version of GLFW (3.4) includes a patch from the waywall project (https://github.com/tesselslate/waywall)
to enable compatibility with Minecraft when used with waywall. The patch is applied from:
https://raw.githubusercontent.com/tesselslate/waywall/be3e018bb5f7c25610da73cc320233a26dfce948/contrib/glfw.patch
EOF

  GLFW_MAINTAINERS="$OUTPUT_DIR/glfw-build/MAINTAINERS.md"
  if [ ! -f "$GLFW_MAINTAINERS" ]; then
    echo "Error: Failed to create $GLFW_MAINTAINERS"
    exit 1
  fi

  echo "GLFW built successfully. Libraries: ${GLFW_FILES[*]}"
  echo "License: $GLFW_LICENSE"
  echo "Contributors: $GLFW_CONTRIBUTORS"
  echo "Maintainers: $GLFW_MAINTAINERS"
  echo "Waywall source: $WAYWALL_DIR"

  # Step 2: Copy GLFW files to waywall/glfw/ directory
  echo "Copying GLFW files to $WAYWALL_DIR/glfw/..."
  mkdir -p "$WAYWALL_DIR/glfw"
  for file in "${GLFW_FILES[@]}"; do
    cp "$file" "$WAYWALL_DIR/glfw/$(basename "$file")"
  done
  cp "$GLFW_LICENSE" "$WAYWALL_DIR/glfw/LICENSE.md"
  cp "$GLFW_CONTRIBUTORS" "$WAYWALL_DIR/glfw/CONTRIBUTORS.md"
  cp "$GLFW_MAINTAINERS" "$WAYWALL_DIR/glfw/MAINTAINERS.md"

  # Verify copied files
  for file in "$WAYWALL_DIR/glfw/libglfw.so" "$WAYWALL_DIR/glfw/libglfw.so.3" "$WAYWALL_DIR/glfw/libglfw.so.3.4" \
              "$WAYWALL_DIR/glfw/LICENSE.md" "$WAYWALL_DIR/glfw/CONTRIBUTORS.md" "$WAYWALL_DIR/glfw/MAINTAINERS.md"; do
    if [ ! -f "$file" ]; then
      echo "Error: Failed to copy $file"
      exit 1
    fi
  done
  echo "GLFW files copied successfully to $WAYWALL_DIR/glfw/"
fi

# Step 3: Build waywall for each distro
for distro_pair in $IMAGE_TAGS; do
  distro=${distro_pair%%:*}
  image_tag=${distro_pair#*:}
  echo "Building waywall for $distro..."

  if [ "$distro" = "archlinux" ]; then
    podman run --rm --pull=never -v "$WAYWALL_DIR:/build/waywall:Z" --workdir /build/waywall --entrypoint /bin/bash localhost/pacur/$image_tag -c "
      pacman -Syu --noconfirm
      pacman -S --noconfirm ninja meson wayland-protocols libegl libgles luajit libspng libxcb libxkbcommon xorg-xwayland
      useradd -m -u 1000 builduser
      echo 'builduser ALL=(ALL) NOPASSWD: ALL' >> /etc/sudoers
      chown -R builduser:builduser /build/waywall
      su builduser -c 'cd /build/waywall && makepkg -sf --noconfirm'
    "
  elif [ "$distro" = "fedora" ]; then
    podman run --rm --pull=never -v "$WAYWALL_DIR:/build/waywall:Z" --workdir /build/waywall --entrypoint /bin/bash localhost/pacur/$image_tag -c "
      dnf install -y libspng-devel cmake meson mesa-libEGL-devel luajit-devel libwayland-client libwayland-server libwayland-cursor libxkbcommon-devel xorg-x11-server-Xwayland-devel wayland-protocols-devel wayland-scanner
      rpmbuild -ba --define '_sourcedir /build/waywall' --define '_srcrpmdir /build/waywall' --define '_rpmdir /build/waywall' --define '_builddir /build/waywall/build' /build/waywall/waywall.spec
      cp /build/waywall/x86_64/*.rpm /build/waywall/
    "
  elif [ "$distro" = "debian" ]; then
    podman run --rm --pull=never -v "$WAYWALL_DIR:/build/waywall:Z" --workdir /build/waywall --entrypoint /bin/bash localhost/pacur/$image_tag -c "
      apt-get update
      apt-get install -y libgles2-mesa-dev libegl-dev pkg-config debhelper-compat wayland-protocols meson build-essential libspng-dev libluajit-5.1-dev libwayland-dev libxkbcommon-dev xwayland cmake wayland-scanner++ libegl1 luajit libspng0 libwayland-client0 libwayland-cursor0 libwayland-egl1 libwayland-server0 libxcb1 libxcb-composite0-dev libxcb-res0-dev libxcb-xtest0-dev xwayland libxkbcommon0
      dpkg-buildpackage -b -us -uc
      cp ../*.deb /build/waywall/
    "
  fi
done

# Clean up GLFW build directory
rm -rf "$OUTPUT_DIR/glfw-build"

# Copy the built packages to the output directory
cp -v "$WAYWALL_DIR"/*.pkg.tar.zst "$OUTPUT_DIR" || true
cp -v "$WAYWALL_DIR"/*.rpm "$OUTPUT_DIR" || true
cp -v "$WAYWALL_DIR"/*.deb "$OUTPUT_DIR" || true

# Remove the built packages from the waywall directory
rm -f "$WAYWALL_DIR"/*.pkg.tar.zst
rm -f "$WAYWALL_DIR"/*.rpm
rm -f "$WAYWALL_DIR"/*.deb

echo "Build process completed successfully for all distros."
echo "Build artifacts are located in: $OUTPUT_DIR"
