# Installation

waywall is available through some distribution-specific package repositories
(presently the Arch User Repository and Nixpkgs.). Additionally, prebuilt
binary packages are provided on the [Releases] page.

**The following sections contain the methods available for installing waywall
on various distributions, listed roughly in order of preference.**

## Arch Linux

waywall is available on Arch-based distributions through the the Arch User
Repository via the [`waywall-working-git`] package.

Prebuilt binary packages are additionally provided on the [Releases] page.

Lastly, waywall can be manually [built from source].

## Debian

Prebuilt binary packages for Debian 13 are provided on the [Releases] page.

> [!CAUTION]
> waywall is unable to run on many Debian derivatives, including Linux Mint and
> most versions of Ubuntu, as their package repositories are too outdated.

<br/>

Additionally, waywall can be manually [built from source].

> [!NOTE]
> If you want to compile from source on Debian 13, you will need to use Clang,
> as the packaged version of GCC is not up-to-date enough.

## Fedora

Prebuilt binary packages for Fedora 42 are provided on the [Releases] page.

Additionally, waywall can be manually [built from source].

## Nix

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

## Building with the packaging script

The package building script is able to create binary packages for Arch Linux,
Debian 13, and Fedora 42.

> [!IMPORTANT]
> This script only builds packages for **Arch Linux**, **Debian**, and
> **Fedora**. If you use another distribution, you will have to
> [build waywall from source](#building-from-source).

This script automatically builds both the main waywall binary and the mandatory
patched version of GLFW, which is located at `/usr/local/lib64/waywall-glfw/libglfw.so`.

### Dependencies
- `podman`
- `git`
- `pacur fedora-42, arch and debian-trixie containers` (from https://github.com/pacur/pacur)
- `docker`

### Container setup

```sh
git clone https://github.com/pacur/pacur
cd pacur/docker
find . -maxdepth 1 -type d \( ! -name "archlinux" ! -name "debian-trixie" ! -name "fedora-42" \) -exec rm -rf {} +
for dir in */ ; do podman build --rm -t "pacur/${dir::-1}" "$dir"; done
```

The containers should now be installed. If the build fails, try rebooting your machine.

### Setup

- Clone waywall repository `git clone https://github.com/tesselslate/waywall`
- Make the main script executable `chmod u+x build-packages.sh`
- [Install pacur containers](#container-setup) for `archlinux`, `fedora-42`, and `debian-trixie`
- Run `./build-packages.sh` inside the waywall directory and select which distributions to build for
  - Within the script: 1 for Arch, 2 for Fedora, 3 for Debian, 4 for done
  - Or, use the provided script flags for building (for example `./build-packages.sh --debian` or `./build-packages.sh --fedora --arch`)
- Enjoy

### Installation

The script will output where the build artifacts are located (for example `Build artifacts are located in: ~/waywall/waywall-build`).
On some distributions, you can double-click the correct built package in your
graphical file manager of choice. Otherwise, install it from the terminal with
one of the following commands:

- ArchLinux: `sudo pacman -U ~/waywall/waywall-build/waywall-0.5-1-x86_64.pkg.tar.zst`
- Fedora: `sudo dnf localinstall ~/waywall/waywall-build/waywall-0.5-1.fc42.x86_64.rpm`
- Debian: `sudo dpkg -i ~/waywall/waywall-build/waywall_0.5-1_amd64.deb`

[built from source]: #building-from-source
[Releases]: https://github.com/tesselslate/waywall/releases
[`waywall-working-git`]: https://aur.archlinux.org/packages/waywall-working-git
