#!/usr/bin/env bash

# usage: ./pkg.sh <distro>
#
# - pkg_<distro>.sh must exist, with its 2nd line being a comment with the name
#   of the docker image to use.
#
# - all packaging scripts require $PWD to be set to the root of the waywall tree
# - all packaging scripts expect /glfw to exist and contain all GLFW files
#     (with the exception of pkg-glfw.sh)
# - all packaging scripts place their outputs in /out

# $1 = name, $2 = image, $3 = script
build() {
    podman run \
        --rm \
        --pull=newer \
        -v=..:/waywall:O \
        -v=.out/glfw:/glfw:ro \
        -v=".out/$1":/out:rw \
        --workdir /waywall \
        "$2" \
        bash "/waywall/contrib/$3"
}

# $1 = prompt
confirm() {
    read -r -p "$1? [y/n] " ANS
    case "$ANS" in
        [yY][eE][sS]|[yY])
            true
            ;;
        *)
            false
            ;;
    esac
}

# ensure script is run with $PWD set to the contrib directory
set -e
DIR="$(dirname "$(realpath "$0")")"
if [ "$DIR" != "$(pwd -P)" ]; then
    echo "pwd must equal '$DIR'"
    if confirm "change directory"; then
        cd "$DIR"
    else
        exit 1
    fi
fi

# check that script exists and extract image name
SCRIPT="pkg_$1.sh"
if [ ! -f "$SCRIPT" ]; then
    echo "$SCRIPT not found"
    exit 1
fi
IMAGE="$(sed -n "2s/# //p;2q" "$SCRIPT")"
echo -e "script:\t$SCRIPT\nimage:\t$IMAGE\n"

# ensure output directories exist
mkdir -p ".out/$1"
mkdir -p ".out/glfw"

# check if glfw is built
GLFW_BUILT=true
for f in "LICENSE.md" "CONTRIBUTORS.md" "PATCHES.md" "libglfw.so" "libglfw.so.3" "libglfw.so.3.4"; do
    if [ ! -f ".out/glfw/$f" ]; then
        GLFW_BUILT=false
        break
    fi
done

if $GLFW_BUILT; then
    echo -e "found GLFW artifacts"
else
    echo "failed to find GLFW artifacts"
    if confirm "build glfw"; then
        build "glfw" "archlinux:base-devel" "pkg-glfw.sh"
    else
        echo "building without GLFW artifacts"
    fi
fi

# run build script in container
build "$1" "$IMAGE" "$SCRIPT"
