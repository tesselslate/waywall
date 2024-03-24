# waywall

waywall is a Wayland compositor that also serves as a reset macro for Minecraft
speedruns. It is designed to be nested within an existing Wayland session and is
intended as a successor to [resetti](https://github.com/tesselslate/resetti).

> [!WARNING]
> waywall is still under active development. Many features are missing or may
> not work as expected.

# Building

The following runtime dependencies are required:

 - `libzip`
 - `luajit`
 - `wayland`
 - `wayland-protocols`
 - `xkbcommon`

To build waywall, clone the repository and run the following commands:

```sh
meson setup build
ninja -C build
```
