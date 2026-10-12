# keyboard_shortcuts_inhibited

This function checks whether the host compositor's shortcuts are inhibited.

The host compositor may choose to temporarily ignore this inhibition,
usually by having the user press some special key combination.
Nevertheless, this function will keep returning `true`, unless the inhibition
is disabled with [`waywall.set_keyboard_shortcuts_inhibition`]
or the `inhibit_keyboard_shortcuts` option in the [window configuration table].

If the host compositor doesn't implement
[keyboard-shortcuts-inhibit-unstable-v1] this will always return `false`.

### Return values

  - `inhibited`: boolean

> This function cannot be called during startup.

[`waywall.set_keyboard_shortcuts_inhibition`]: 02_waywall_set_keyboard_shortcuts_inhibition.md
[window configuration table]: 01_options_window.md
[keyboard-shortcuts-inhibit-unstable-v1]: https://wayland.app/protocols/keyboard-shortcuts-inhibit-unstable-v1
