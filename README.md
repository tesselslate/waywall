# waywall

waywall is a Wayland compositor that also serves as a reset macro for Minecraft
speedruns. It uses wlroots and is intended to be nested within an existing
Wayland session. It is intended as a successor to [resetti](https://github.com/tesselslate/resetti).

> **Warning**
> waywall is still very much in development. There are bound to be bugs and
> unexpected behavior.

# Building

```
$ git clone https://github.com/tesselslate/waywall
$ cd waywall
$ meson setup build
$ ninja -C build
```

You will need the following dependencies:

- `libxcb`
- `libxkbcommon`
- `libwayland`
- `libzip`
- `wayland-protocols`
- OBS (`libobs`)

# Configuration

Copy the `config.toml` file to `$XDG_CONFIG_HOME/waywall.toml` and modify it as
you like.

# Usage

Run `waywall` to start the compositor. Launch your instances with the
`waywall-launch` wrapper program to spawn them inside of the compositor.

If you use the `remain_in_background` configuration option, the waywall window
can be reopened by sending SIGUSR1 to the waywall process.
