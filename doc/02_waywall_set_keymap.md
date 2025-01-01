# set_keymap

This function attempts to change the current XKB layout in use by Minecraft.
The following keys will be read from the options table if they are set:

  - `layout`
  - `model`
  - `rules`
  - `variant`
  - `options`

These keys behave the same as those in the [input configuration table].

### Arguments

  - `options`: table

### Return values

None

> This function cannot be called during startup.

[input configuration table]: 01_options_input.md
