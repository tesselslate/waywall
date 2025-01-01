# text

This function creates and displays text over top of the waywall window.

## Text object

This function will return an object which can be used to remove the text from
the waywall window at a later point. The only method made available by the
returned text object is `close`.

If this object is not stored in a permanent location, the text may randomly
disappear when it gets garbage collected.

### Arguments

  - `text`: string
  - `x`: number
  - `y`: number
  - `color`: string (optional)
  - `size`: number (optional)
  - `shader`: string (optional)

### Return values

  - `text`: table

> This function cannot be called during startup.
