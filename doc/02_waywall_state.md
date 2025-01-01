# state

This function returns a table describing the current state of the Minecraft
instance according to the State Output mod. If the instance does not have
State Output installed and enabled, this function will throw an error when
called.

The returned table may have one of several structures depending on the state
of the instance. The `screen` field will always be present, allowing you to
determine which form it has:

### `title`, `waiting`, and `wall`

```lua
{
    screen = "title" or "waiting" or "wall",
}
```

### `generating` and `previewing`

```lua
{
    screen = "generating" or "previewing",
    percent = 0,
}
```

### `inworld`

```lua
{
    screen = "inworld",
    inworld = "unpaused" or "paused" or "menu",
}
```

### Arguments

None

### Return values

  - `state`: table

> This function cannot be called during startup.
