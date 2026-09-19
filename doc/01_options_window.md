# Window

The `window` section of the configuration table allows you to configure aspects
of the window that waywall opens.

## Default values

```lua
local config = {
    window = {
        fullscreen_width = 0,
        fullscreen_height = 0,
        inhibit_keyboard_shortcuts = false,
    },
}

return config
```

## Fullscreen resolution

The `fullscreen_width` and `fullscreen_height` options allow you to specify a
width and height which waywall should force the game to render at while the game
is fullscreened.

This option is especially useful if you have display scaling enabled (i.e. 110%
at 1440p) but would still like Minecraft to render at a normal resolution.

If either value is 0, then waywall will use whichever resolution the compositor
tells it to use.

## Inhibiting keyboard shortcuts

The `inhibit_keyboard_shortcuts` allows you to request the host compositor
to ignore its keyboard shortcuts (like <kbd><kbd>Alt</kbd>+<kbd>Tab</kbd></kbd>
or <kbd>Super</kbd>) and pass these key events to waywall.

This option requires your compositor to support the
[keyboard-shortcuts-inhibit-unstable-v1] protocol,
or else it will have no effect.

[keyboard-shortcuts-inhibit-unstable-v1]: https://wayland.app/protocols/keyboard-shortcuts-inhibit-unstable-v1
