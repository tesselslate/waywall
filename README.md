# waywall [![Discord](https://img.shields.io/discord/1095808506239651942?style=flat-square)](https://discord.gg/3tm4UpUQ8t)

waywall is a Wayland compositor that provides various convenient features (key
rebinding, Ninjabrain Bot support, etc) for Minecraft speedrunning. It is
designed to be nested within an existing Wayland session and is intended as a
successor to [resetti](https://github.com/tesselslate/resetti).

> [!NOTE]
> waywall is still under development. Some features are missing or may
> not work as expected.

# Installation

waywall is available through some package managers, and prebuilt binary packages
are provided for various popular distributions on the [Releases](https://github.com/tesselslate/waywall/releases)
page.

For more detailed installation instructions, refer to the [documentation](https://tesselslate.github.io/waywall/00_installation.html).

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
