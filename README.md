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

Users on other distributions must build waywall from source or use bundled build-packages.sh.

## Building from build-packages.sh (Debian, Fedora and Arch, includes patched GLFW)

This script includes the patched GLFW, it will always be located on ```/usr/local/lib64/waywall-glfw/libglfw.so```

### Dependencies: 
- `podman`
- `git`
- `pacur fedora-42, arch and debian-trixie containers` (from https://github.com/pacur/pacur)
- `docker`
- `go`

### Setup:

- Clone waywall repository ```git clone https://github.com/tesselslate/waywall```
- Install pacur containers for **archlinux** **fedora-42** and **debian-trixie**, ensure they are installed for your user, not root, remove sudo on build.sh /docker/
- Run ```./build-packages.sh``` inside waywall folder and select the distro family you want to build for (1 for arch, 2 for fedora, 3 for debian, 4 for done) or use the provided script flags for building (for example ```./build-packages.sh --debian``` ```./build-packages.sh --fedora --arch```)
- Enjoy

### Steps for installing pacur containers:

- ```go install github.com/pacur/pacur@latest```
- ```cd ~/go/pkg/mod/github.com/pacur/pacur@(version)/docker/``` (You can check the version doing ```ls  ~/go/pkg/mod/github.com/pacur/``` or from a file manager)
- ```sudo find . -maxdepth 1 -type d \( ! -name "archlinux" ! -name "debian-trixie" ! -name "fedora-42" \) -exec rm -rf {} +```
- open the build.sh file and remove any ```su``` or ```sudo``` from the file to build containers as **user** (Use ```sudo nano build.sh``` for editing the file or ```sudo vim build.sh```) 
- ```./build.sh```
- Done, containers should now be installed, if it still doesn't build do a reboot

### Steps for installing the built waywall:

- The script will output where the build is located (for example ```Build artifacts are located in: ~/waywall/waywall-build``` depending on the distro you are currently you can either just double-click the file or install it from terminal

### Example commands for installing waywall:

- ArchLinux: ```sudo pacman -U ~/waywall/waywall-build/waywall-0.5-1-x86_64.pkg.tar.zst```
- Fedora: ```sudo dnf localinstall ~/waywall/waywall-build/waywall-0.5-1.fc42.x86_64.rpm```
- Debian: ```sudo dpkg -i ~/waywall/waywall-build/waywall_0.5-1_amd64.deb```

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
