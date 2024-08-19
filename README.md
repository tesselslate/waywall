# waywall [![Discord](https://img.shields.io/discord/1095808506239651942?style=flat-square)](https://discord.gg/fwZA2VJh7k)

waywall is a Wayland compositor that provides various convenient features (key
rebinding, Ninjabrain Bot support, etc) for Minecraft speedrunning. It is
designed to be nested within an existing Wayland session and is intended as a
successor to [resetti](https://github.com/tesselslate/resetti).

> [!NOTE]
> waywall is still under active development. Some features are missing or may
> not work as expected.

# Building

The following dependencies are required only at build time:

- `wayland-protocols`

The following dependencies are required for both building and running:

 - `luajit`
 - `wayland-client`
 - `wayland-cursor`
 - `wayland-server`
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
