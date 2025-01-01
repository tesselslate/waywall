# listen

This function registers the given event listener to be called whenever the given
event occurs. The returned cancellation function can be called to unregister the
event listener so that it is not triggered anymore.

### Arguments

  - `event`: string
  - `listener`: function

### Return values

  - `cancel`: function
