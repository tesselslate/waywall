# Input

The `input` section of the configuration table allows you to configure how
keyboard and mouse input is processed by waywall in a number of ways.

## Default values

```lua
local config = {
    input = {
        -- XKB keymap options
        layout = "",
        model = "",
        rules = "",
        variant = "",
        options = "",

        -- key/button remappings
        remaps = {},

        -- key repeat
        repeat_rate = -1,
        repeat_delay = -1,

        -- mouse options
        sensitivity = 1.0,
        confine_pointer = false,
    },
}

return config
```

## Keymap configuration

waywall allows you to use a different keyboard layout from the rest of your
Wayland session (e.g. for search crafting with [different languages]).

There are five options in the `input` table which can be used to configure which
keymap is loaded by XKB:

> `layout`, `model`, `rules`, `variant`, `options`

Unfortunately, detailed information on XKB configuration is outside the scope of
this document. You can refer to the [libxkbcommon documentation] for more
information. If installed, you can also refer to the standard database of XKB
configuration rules, which is typically located at `/usr/share/X11/xkb`.

## Input remapping

waywall allows you to remap keys and mouse buttons to one another in any
fashion via the `remaps` table within the `input` table. Here are a few
examples:

```lua
{
    ["MB4"] = "Home",   -- remap side button to Home
    ["X"] = "F3",       -- remap X to F3
}
```

Each key-value pair specifies a single remapping, where the key is the source
input (button or key) and the value is the resulting output which gets sent to
the game.

The list of all available keycodes and mouse buttons can be found in the
[Lookup Tables] section.

## Key repeat

waywall allows you to configure how key repeat works with the `repeat_rate`
and `repeat_delay` options.

The `repeat_rate` option specifies how many times per second a keypress should
be repeated while it is held down. This option is mostly useful for increasing
the speed at which the F3+F key combination changes your render distance.

The `repeat_delay` option determines how long (in milliseconds) a key must be
held down before it will start repeating.

By default, both options are set to a value of `-1`, which means they will use
the same values as your main Wayland session.

> [!WARNING]
> If you use a short `repeat_delay` and set `repeat_rate` to more than 20 Hz,
> you may experience issues with switching hotbar slots. This is due to a bug in
> how the game handles certain keybinds and can be avoided by either using a
> longer `repeat_delay` or using a `repeat_rate` no greater than 20 Hz.

## Mouse sensitivity

The `sensitivity` option applies a constant multiplier to your mouse motion
while aiming with the camera ingame. The default value of `1.0` results in
no change, while `2.0` would make it twice as fast and `0.5` would make it
half as fast.

> [!IMPORTANT]
> This option only affects your camera movement, *not* your mouse movement in
> menus. Additionally, it only takes effect **when Raw Input is disabled ingame.**

## Pointer confinement

The `confine_pointer` option allows you to lock your mouse pointer to the
waywall window. This can be useful if you have two monitors and accidentally
flick your mouse to the side while in your inventory, which might otherwise
cause you to focus a different window and lose time.

[different languages]: https://docs.google.com/spreadsheets/d/1NM5U84PjTBA6oMDSyFgveVVWcP7i0aCIFqvQCNUotYE/edit?usp=sharing
[libxkbcommon documentation]: https://xkbcommon.org/doc/current/user-configuration.html
[Lookup Tables]: 03_lookup_tables.md
