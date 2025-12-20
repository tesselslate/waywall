# waywall [![Discord](https://img.shields.io/discord/1095808506239651942?style=flat-square)](https://discord.gg/3tm4UpUQ8t)

waywall is a Wayland compositor that provides various convenient features (key
rebinding, Ninjabrain Bot support, etc) for Minecraft speedrunning. It is
designed to be nested within an existing Wayland session and is intended as a
successor to [resetti](https://github.com/tesselslate/resetti).

> [!NOTE]
> waywall is still under development. Some features are missing or may
> not work as expected.

# Installation

Distribution-specific packages are currently only available for Arch through
the AUR.

  - [Arch Linux (AUR)](https://aur.archlinux.org/packages/waywall-working-git)

Users on other distributions must [build waywall from source](#building-from-source),
download a prebuilt package from the [Releases](https://github.com/tesselslate/waywall/releases)
page, or [use the `build-packages.sh` script](#building-with-build-packagessh) to build their own.

## Building with `build-packages.sh`

> [!IMPORTANT]
> This script only builds packages for **Arch Linux**, **Debian**, and
> **Fedora**. If you use another distribution, you will have to
> [build waywall from source](#building-from-source).

This script automatically builds both the main waywall binary and the mandatory
patched version of GLFW, which is located at `/usr/local/lib64/waywall-glfw/libglfw.so`.

### Dependencies:
- `podman`
- `git`
- `pacur fedora-42, arch and debian-trixie containers` (from https://github.com/pacur/pacur)
- `docker`

### Setup:

- Clone waywall repository `git clone https://github.com/tesselslate/waywall`
- Make the main script executable `chmod u+x build-packages.sh`
- [Install pacur containers](#steps-for-installing-pacur-containers) for `archlinux`, `fedora-42`, and `debian-trixie`
- Run `./build-packages.sh` inside the waywall directory and select which distributions to build for
  - Within the script: 1 for Arch, 2 for Fedora, 3 for Debian, 4 for done
  - Or, use the provided script flags for building (for example `./build-packages.sh --debian` or `./build-packages.sh --fedora --arch`)
- Enjoy

### Steps for installing pacur containers:

```sh
git clone https://github.com/pacur/pacur
cd pacur/docker
find . -maxdepth 1 -type d \( ! -name "archlinux" ! -name "debian-trixie" ! -name "fedora-42" \) -exec rm -rf {} +
for dir in */ ; do podman build --rm -t "pacur/${dir::-1}" "$dir"; done
```

The containers should now be installed. If the build fails, try rebooting your machine.

### Steps for installing the built waywall:

The script will output where the build artifacts are located (for example `Build artifacts are located in: ~/waywall/waywall-build`).
On some distributions, you can double-click the correct built package in your
graphical file manager of choice. Otherwise, install it from the terminal with
one of the following commands:

- ArchLinux: `sudo pacman -U ~/waywall/waywall-build/waywall-0.5-1-x86_64.pkg.tar.zst`
- Fedora: `sudo dnf localinstall ~/waywall/waywall-build/waywall-0.5-1.fc42.x86_64.rpm`
- Debian: `sudo dpkg -i ~/waywall/waywall-build/waywall_0.5-1_amd64.deb`

## Building from source

The following dependencies are required only at build time:

 - `wayland-protocols`

The following dependencies are required for both building and running:

 - `egl`
 - `glesv2`
 - `luajit`
 - `spng`
 - `wayland-client`
 - `wayland-cursor`
 - `wayland-egl`
 - `wayland-server`
 - `xcb`
 - `xcb-composite`
 - `xcb-res`
 - `xcb-xtest`
 - `xwayland`
 - `xkbcommon`

To build waywall, clone the repository and run `make`.

# License

waywall is licensed under the GNU General Public License v3 **only**, no later
version.

# Attributions

Non-trivial code has been referenced and reused from the following repositories,
particularly for Xwayland support. Their licenses can be found in the relevant
files.

- [weston](https://gitlab.freedesktop.org/wayland/weston)
- [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)

waywall bundles a version of the [Terminus Font](https://terminus-font.sourceforge.net/),
which is licensed under the Open Font License. See `include/util/font.h` for
more information.

waywall bundles a patched version of [GLFW](https://github.com/glfw/glfw) *if built
with the included building script*, GLFW is licensed under the ZLib License. See `/usr/share/doc/waywall/glfw-LICENSE.md`
& `/usr/share/doc/waywall/glfw-MAINTAINERS.md` for more information.
