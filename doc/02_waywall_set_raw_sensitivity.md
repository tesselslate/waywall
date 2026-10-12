# set_raw_sensitivity

This function changes the mouse sensitivity multiplier which is used for
camera movement when using raw input (unaccelerated by the host compositor).
If the provided sensitivity is 0, the sensitivity will instead be reset
to whatever value you have specified in the [input configuration table].

### Arguments

  - `sensitivity`: number

### Return values

None

> This function cannot be called during startup.

[input configuration table]: 01_options_input.md
