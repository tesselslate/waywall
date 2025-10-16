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

### Arguments

  - `text`: string
  - `options`: table

### Return values

  - `text`: [text object]

> This function cannot be called during startup.

[text object]: 02_type_text.md
