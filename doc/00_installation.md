# Installation

waywall is available on Arch through the AUR and on systems with Nix through Nixpkgs.

  - [Arch Linux (AUR)](https://aur.archlinux.org/packages/waywall-working-git)
  - [Nix (Nixpkgs)](https://search.nixos.org/packages?channel=unstable&query=waywall)

Users on other distributions must download a prebuilt package from the
[Releases](https://github.com/tesselslate/waywall/releases) page or build
waywall from source.

## Installing with Nix

waywall is available in Nixpkgs since 26.05.

If it is available, waywall should be installed on your NixOS or Home Manager profile.

### NixOS

```nix
# configuration.nix
{ pkgs, ... }:
{
  environment.systemPackages = [
    pkgs.prismlauncher
    pkgs.waywall
  ];
}
```

### Home Manager

```nix
# home.nix
{ pkgs, ... }:
{
  home.packages = [
    pkgs.prismlauncher
    pkgs.waywall
  ];
}
```

### Nix Profile

On other distributions with Nix installed, it can be installed with:

```
$ nix profile install nixpkgs#waywall
```

## Building with the packaging script

The package building script is able to create binary packages for Arch Linux,
Debian 13, and Fedora 42.

Refer to the instructions within the [README](https://github.com/tesselslate/waywall/#building-with-build-packagessh)
for more information on how to use the packaging script.

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

> [!IMPORTANT]
> Many distributions, such as Fedora and Debian, split the "development files"
> (e.g. pkg-config data and C headers) into separate `-dev` or `-devel` packages.
> Make sure to find and install these in addition to the normal versions.

### Compiling

You can download and build a copy of waywall with the following commands:

```sh
git clone https://github.com/tesselslate/waywall
cd waywall
make
```

The compiled binary will be located at `build/waywall/waywall`. If you'd like,
you can move it to somewhere on your `$PATH`.
