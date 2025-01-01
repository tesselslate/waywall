# sleep

This function causes the current Lua execution context to pause for the given
number of milliseconds. Other Lua code, such as other keybind or event handlers,
can be executed while the current execution context is paused.

Calling this function forbids keybind handlers from marking an input as
non-consumed. See [Input consumption] for more details.

<div class="warning">

Currently, it is only allowed to call this function from within a keybind
handler. Support for calling this function from an event listener may be added
at a later date.

</div>

### Arguments

  - `ms`: number

### Return values

None

> This function cannot be called during startup.

[Input consumption]: 01_options_actions.md#input-consumption
