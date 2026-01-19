# Window

The `window` section of the configuration table allows you to configure aspects
of the window that waywall opens.

## Default values

```lua
local config = {
    window = {
        fullscreen_width = 0,
        fullscreen_height = 0,
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
