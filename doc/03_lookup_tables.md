# Lookup Tables

waywall has pre-defined sets of names which correspond to keys, mouse buttons,
and modifiers. These are used when loading your configuration. All names are
case insensitive.

## Keycodes

> [!IMPORTANT]
> Keycodes are used for [remapping keys and buttons](01_options_input.md)
> in your configuration and for [pressing keys in actions](02_waywall_press_key.md).

The list of valid keycodes can be found in waywall's source code
[here](https://github.com/tesselslate/waywall/blob/main/include/util/keycodes.h).

Virtually all normal keyboard keys are present, although some may not be named
as you expect. waywall follows the names defined by the `input-event-codes.h`
header from Linux.

## Keysyms

> [!IMPORTANT]
> Keysyms are used for adding [actions](01_options_actions.md) to your
> configuration.

The list of valid keysyms comes from libxkbcommon. The [`xkbcommon-keysyms.h`]
header contains all of the valid keysym names. Each keysym's name is prefixed
with `XKB_KEY_`.

## Modifiers

The following table lists all valid names for modifiers:

| Modifier      | Names   |           |            |
|---------------|---------|-----------|------------|
| **Shift**     | `shift` |           |            |
| **Control**   | `ctrl`  | `control` |            |
| **Caps Lock** | `caps`  | `lock`    | `capslock` |
| **Alt**       | `mod1`  | `alt`     |            |
| **Num Lock**  | `mod2`  | `num`     | `numlock`  |
| **Mod3**      | `mod3`  |           |            |
| **Super**     | `mod4`  | `super`   | `win`      |
| **Mod5**      | `mod5`  |           |            |

## Mouse buttons

The following table lists all valid names for mouse buttons:

| Button               | Names |      |          |               |
|----------------------|-------|------|----------|---------------|
| **Left Button**      | `lmb` | `m1` | `mouse1` | `leftmouse`   |
| **Middle Button**    | `mmb` | `m3` | `mouse3` | `middlemouse` |
| **Right Button**     | `rmb` | `m2` | `mouse2` | `rightmouse`  |
| **Side button (M4)** | `mb4` | `m4` | `mouse4` |               |
| **Side button (M5)** | `mb5` | `m5` | `mouse5` |               |

[`xkbcommon-keysyms.h`]: https://github.com/xkbcommon/libxkbcommon/blob/master/include/xkbcommon/xkbcommon-keysyms.h
