# Installation

Distribution-specific packages are currently only available for Arch through
the AUR.

  - [Arch Linux (AUR)](https://aur.archlinux.org/packages/waywall-working-git)

Users on other distributions must build waywall from source.

## Building from source

waywall is written in C and uses the Meson build system, so you will need to
install a C toolchain and `meson` if they are not already on your system.

You will also need to install the following dependencies from your
distribution's package repositories:

  - `egl`
  - `glesv2`
  - `luajit`
  - `spng`
  - `wayland-client`
  - `wayland-cursor`
  - `wayland-egl`
  - `wayland-protocols`
  - `wayland-server`
  - `xcb`
  - `xcb-composite`
  - `xcb-res`
  - `xcb-xtest`
  - `xwayland`
  - `xkbcommon`

<div class="warning">

Many distributions, such as Fedora and Debian, split the "development files"
(e.g. pkg-config data and C headers) into separate `-dev` or `-devel` packages.
Make sure to find and install these in addition to the normal versions.

</div>

### Compiling

You can download and build a copy of waywall with the following commands:

```sh
git clone https://github.com/tesselslate/waywall
cd waywall
make
```

The compiled binary will be located at `build/waywall/waywall`. If you'd like,
you can move it to somewhere on your `$PATH`.
