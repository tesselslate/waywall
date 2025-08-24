# text

This function creates and displays text over top of the waywall window.

The `options` table can have the following options, of which `x` and `y` are
non-optional:

```lua
{
    -- absolute location of text in waywall window
    x = 100,
    y = 100,

    -- color of text (optional)
    color = "#abcdef",

    -- size of text (optional)
    size = 1,

    -- optional
    depth = 0,

    -- optional
    shader = "shader_name"
}
```

## Text object

This function will return an object which can be used to remove the text from
the waywall window at a later point. The only method made available by the
returned text object is `close`.

If this object is not stored in a permanent location, the text may randomly
disappear when it gets garbage collected.

### Arguments

  - `text`: string
  - `options`: table

### Return values

  - `text`: table

> This function cannot be called during startup.
