# listen

This function registers the given event listener to be called whenever the given
event occurs. The returned cancellation function can be called to unregister the
event listener so that it is not triggered anymore.

The list of valid event names is:

  - `load`
    - For when configuration loading is finished and functions such as
      `waywall.text()` are now legal to call
  - `resolution`
    - For resolution changes with `waywall.set_resolution()`
  - `state`
    - For updates to the instance's state-output file

### Arguments

  - `event`: string
  - `listener`: function

### Return values

  - `cancel`: function
