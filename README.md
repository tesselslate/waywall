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

## Building from build-packages.sh (Debian, Fedora and Arch)

Dependencies: 
- `podman`
- `git`
- `pacur fedora42, arch and debian-trixie containers` (from https://github.com/pacur/pacur)
- `docker`

Setup:

- Clone waywall repository (git clone https://github.com/tesselslate/waywall)
- Install pacur containers for **archlinux** **fedora-42** and **debian-trixie**, ensure they are installed for your user, not root, remove sudo on build.sh /docker/
- Run build-packages.sh inside waywall folder
- Enjoy

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
