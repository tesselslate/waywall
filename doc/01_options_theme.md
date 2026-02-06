# Theme

The `theme` section of the configuration table allows you to configure the
appearance of waywall.

## Default values

```lua
local config = {
    theme = {
        background = "#000000ff",
        background_png = "",

        cursor_theme = "",
        cursor_icon = "",
        cursor_size = 0,

        ninb_anchor = "",
        ninb_opacity = 1.0,
    },
}

return config
```

## Background

The `background` or `background_png` option determines the background of the
waywall window. `background_png` specifies the path to a PNG file, otherwise
`background` determines the solid color background. The background is only
visible while the Minecraft window does not occupy the entire waywall window
(e.g. while using Thin BT or boat eye.)

For `background`, you may specify either an RGB or RGBA hex color. Different
compositors may handle non-opaque background colors differently, and non-opaque
colors may not appear correctly if waywall is fullscreened.

## Cursor theme

waywall provides three options for configuring the appearance of the cursor:

> `cursor_theme`, `cursor_icon`, `cursor_size`

By default, these options are left unset, and waywall will attempt to
automatically detect and use the cursor settings of your main Wayland session.

The `cursor_theme` option should contain the name of an installed icon theme
(typically in `/usr/share/icons` or `~/.local/share/icons`). The `cursor_icon`
option should point to a valid Xcursor file within the specified theme.

> On some compositors, such as mutter (GNOME), waywall's automatic cursor theme
> detection will fail. waywall attempts to use the `XCURSOR_*` environment
> variables and [`cursor_shape_v1`] protocol for picking a cursor theme, but
> GNOME does not support either of these mechanisms and instead only exposes a
> GNOME-specific DBus interface.

## Ninjabrain Bot

There are two options for changing the appearance of Ninjabrain Bot:

> `ninb_anchor`, `ninb_opacity`

If set, the `ninb_anchor` option will cause the Ninjabrain Bot window to be
locked to a specific side or corner of the waywall window. The following are
valid values for `ninb_anchor`:

  - `topleft`
  - `top`
  - `topright`
  - `left`
  - `right`
  - `bottomleft`
  - `bottomright`

You can additionally provide an offset to `ninb_anchor` by appending `+(x,y)`.
For example, a value of `right+(-20, 20)` will anchor Ninjabrain Bot 20 pixels
to the left and 20 pixels below the center right side of the window.

<br/>

The `ninb_opacity` option allows you to make the Ninjabrain Bot window
translucent. The default value of `1.0` results in a fully opaque window, while
values between `0.0` and `1.0` will result in varying degrees of translucency.

> The `ninb_opacity` option requires that your compositor supports the
> [`alpha_modifier_v1`] protocol. If it is not supported, the option will have
> no effect.

[`cursor_shape_v1`]: https://wayland.app/protocols/cursor-shape-v1
[`alpha_modifier_v1`]: https://wayland.app/protocols/alpha-modifier-v1
