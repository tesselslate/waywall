# toggle_res

This function returns another function which, when called, toggles between the
provided resolution and no resolution (stretching Minecraft back to the bounds
of the waywall window).

If a `number` value is provided for the `sens` and/or `raw_sens` parameters,
toggling to the given resolution will set the mouse sensitivity to those values.
Toggling away from the resolution will set the sensitivity back to the default
values specified in the [input configuration table].

### Arguments

  - `width`: number
  - `height`: number
  - `sens`: number (optional)
  - `raw_sens`: number (optional)

### Return values

  - `toggle`: function

> The returned function cannot be called during startup.

[input configuration table]: 01_options_input.md
