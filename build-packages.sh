#!/bin/bash

set -e  # Exit on errors

WAYWALL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${WAYWALL_OUTPUT_DIR:-$(realpath "$WAYWALL_DIR/waywall-build")}"
mkdir -p "$OUTPUT_DIR"

DISTROS="archlinux fedora debian"
IMAGE_TAGS="archlinux:archlinux fedora:fedora-42 debian:debian-trixie"

# Parse arguments to determine which distros to build
build_distros=()
while [[ $# -gt 0 ]]; do
  case $1 in
    --arch)
      build_distros+=("archlinux")
      shift
      ;;
    --fedora)
      build_distros+=("fedora")
      shift
      ;;
    --debian)
      build_distros+=("debian")
      shift
      ;;
    *)
      echo "Unknown option: $1"
      echo "Usage: $0 [--arch] [--fedora] [--debian]"
      exit 1
      ;;
  esac
done

# If no distros specified, prompt for multi-selection
if [ ${#build_distros[@]} -eq 0 ]; then
  echo "Select distros to build for (select one at a time, then choose 'done' to finish):"
  while true; do
    select distro in $DISTROS done; do
      if [ "$distro" = "done" ]; then
        break 2  # Exit both the select and the while loop
      elif [[ -n "$distro" ]] && [[ ! " ${build_distros[*]} " =~ " $distro " ]]; then
        build_distros+=("$distro")
        echo "Added $distro (current selections: ${build_distros[*]})"
        break  # Back to while loop for next selection
      elif [[ -n "$distro" ]]; then
        echo "Already selected: $distro. Choose another or 'done'."
        break
      else
        echo "Invalid selection. Please try again."
        break
      fi
    done
  done
fi

if [ ${#build_distros[@]} -eq 0 ]; then
  echo "No distros selected. Exiting."
  exit 1
fi


# Define cleanup and copy function
cleanup_and_copy() {
  local is_error=${1:-false}

  echo "Running cleanup and copy..."
  # Clean up building permissions artifacts (arch specific)
  chown -R "$(id -u):$(id -g)" "$WAYWALL_DIR" || true

  # Clean up GLFW build directory (if any)
  rm -rf "$OUTPUT_DIR/glfw-build" || true

  # Copy the built packages to the output directory (if any) - quiet if no matches
  shopt -s nullglob
  for f in "$WAYWALL_DIR"/contrib/*.pkg.tar.zst; do
    cp -v "$f" "$OUTPUT_DIR"
  done
  for f in "$WAYWALL_DIR"/contrib/x86_64/*.rpm; do
    cp -v "$f" "$OUTPUT_DIR"
  done
  for f in "$WAYWALL_DIR"/contrib/*.rpm; do
    cp -v "$f" "$OUTPUT_DIR"
  done
  for f in "$WAYWALL_DIR"/contrib/*.deb; do
    cp -v "$f" "$OUTPUT_DIR"
  done
  shopt -u nullglob

  # Clean up building folders from all distros (if any)
  rm -rf "$WAYWALL_DIR"/contrib/x86_64/ || true
  rm -rf "$WAYWALL_DIR"/contrib/pkg/ || true
  rm -rf "$WAYWALL_DIR"/contrib/src/ || true
  rm -rf "$WAYWALL_DIR"/obj-x86_64-linux-gnu/ || true

  # Remove the built packages from the waywall directory (if any)
  rm -f  "$WAYWALL_DIR"/contrib/*.pkg.tar.zst || true
  rm -f  "$WAYWALL_DIR"/contrib/*.rpm || true
  rm -f  "$WAYWALL_DIR"/contrib/*.deb || true
  rm -rf "$WAYWALL_DIR"/debian/ || true
  rm -f  "$WAYWALL_DIR"/debian-build.patch || true

  if [[ $is_error == 1 ]]; then
    echo ""
    echo "====================================== BUILD ERROR ======================================"
    echo "The script encountered an error. To help fix it, please create a GitHub issue at https://github.com/tesselslate/waywall/issues"
    echo "Include: this full log output, your distro selection, and host OS details."
    echo "========================================================================================="
    echo ""
  else
    echo "Build process completed successfully for all selected distros."
    echo "Build artifacts are located in: $OUTPUT_DIR"
  fi
}

# Set traps to run cleanup on error or exit
ERROR_FLAG=0
trap 'ERROR_FLAG=1' ERR
trap 'cleanup_and_copy $ERROR_FLAG' EXIT

# Check if GLFW files already exist
GLFW_SKIP=true
for file in "$OUTPUT_DIR/glfw/MAINTAINERS.md" "$OUTPUT_DIR/glfw/CONTRIBUTORS.md" "$OUTPUT_DIR/glfw/LICENSE.md" "$OUTPUT_DIR/glfw/libglfw.so" "$OUTPUT_DIR/glfw/libglfw.so.3" "$OUTPUT_DIR/glfw/libglfw.so.3.4"; do
  if [ ! -f "$file" ]; then
    GLFW_SKIP=false
    break
  fi
done

if $GLFW_SKIP; then
  echo "GLFW files already exist in $OUTPUT_DIR/glfw/"
  echo "Skipping GLFW build and copy."
  echo "If you want to rebuild GLFW, please remove the existing files in $OUTPUT_DIR/glfw/."
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

  # Step 2: Copy GLFW files to output directory
  echo "Copying GLFW files to $OUTPUT_DIR/glfw/..."
  mkdir -p "$OUTPUT_DIR/glfw"
  for file in "${GLFW_FILES[@]}"; do
    cp "$file" "$OUTPUT_DIR/glfw/$(basename "$file")"
  done
  cp "$GLFW_LICENSE" "$OUTPUT_DIR/glfw/LICENSE.md"
  cp "$GLFW_CONTRIBUTORS" "$OUTPUT_DIR/glfw/CONTRIBUTORS.md"
  cp "$GLFW_MAINTAINERS" "$OUTPUT_DIR/glfw/MAINTAINERS.md"

  # Verify copied files
  for file in "$OUTPUT_DIR/glfw/libglfw.so" "$OUTPUT_DIR/glfw/libglfw.so.3" "$OUTPUT_DIR/glfw/libglfw.so.3.4" \
              "$OUTPUT_DIR/glfw/LICENSE.md" "$OUTPUT_DIR/glfw/CONTRIBUTORS.md" "$OUTPUT_DIR/glfw/MAINTAINERS.md"; do
    if [ ! -f "$file" ]; then
      echo "Error: Failed to copy $file"
      exit 1
    fi
  done
  echo "GLFW files copied successfully to $OUTPUT_DIR/glfw/"
fi

# Step 3: Build waywall for each distro
for distro in "${build_distros[@]}"; do
  # Find the corresponding image tag
  image_tag=""
  for distro_pair in $IMAGE_TAGS; do
    if [[ $distro_pair == "$distro:"* ]]; then
      image_tag=${distro_pair#*:}
      break
    fi
  done
  if [[ -z "$image_tag" ]]; then
    echo "Error: No image tag found for distro '$distro'"
    continue
  fi
  echo "Building waywall for $distro..."

  if [ "$distro" = "archlinux" ]; then
    podman run --rm --pull=never -v "$WAYWALL_DIR:/build/waywall:Z" --workdir /build/waywall/contrib --entrypoint /bin/bash localhost/pacur/$image_tag -c "
      pacman -Syu --noconfirm
      pacman -S --noconfirm ninja meson wayland-protocols libegl libgles luajit libspng libxcb libxkbcommon xorg-xwayland
      chown -R 65534:65534 /build/waywall
      runuser -u nobody -- sh -c 'makepkg -sf --noconfirm'
      chown -R 0:0 /build/waywall
    "
  elif [ "$distro" = "fedora" ]; then
    podman run --rm --pull=never -v "$WAYWALL_DIR:/build/waywall:Z" --workdir /build/waywall/contrib --entrypoint /bin/bash localhost/pacur/$image_tag -c "
      dnf install -y libspng-devel cmake meson mesa-libEGL-devel luajit-devel libwayland-client libwayland-server libwayland-cursor libxkbcommon-devel xorg-x11-server-Xwayland-devel wayland-protocols-devel wayland-scanner
      rpmbuild -ba --define '_sourcedir /build/waywall' --define '_srcrpmdir /build/waywall/contrib' --define '_rpmdir /build/waywall/contrib' --define '_builddir /build/waywall/build' /build/waywall/contrib/waywall.spec
    "
  elif [ "$distro" = "debian" ]; then
    podman run --rm --pull=never -v "$WAYWALL_DIR:/build/waywall:Z" --workdir /build/waywall --entrypoint /bin/bash localhost/pacur/$image_tag -c "
      apt-get update
      apt-get install -y libgles2-mesa-dev libegl-dev pkg-config debhelper-compat wayland-protocols meson build-essential libspng-dev libluajit-5.1-dev libwayland-dev libxkbcommon-dev xwayland cmake wayland-scanner++ libegl1 luajit libspng0 libwayland-client0 libwayland-cursor0 libwayland-egl1 libwayland-server0 libxcb1 libxcb-composite0-dev libxcb-res0-dev libxcb-xtest0-dev libxkbcommon0 curl git
      cp -r /build/waywall/contrib/debian /build/waywall/
      cp /build/waywall/contrib/debian-build.patch /build/waywall/
      git apply debian-build.patch
      dpkg-buildpackage -b -us -uc
      git apply -R debian-build.patch
      cp ../*.deb /build/waywall/contrib/
    "
  fi
done