# mirror

This function creates a "mirror" which displays a specific area of the Minecraft
window on top of the waywall window. You may want to use it for e.g. boat eye
measurement.

The `options` table can have the following options, although only `src` and
`dst` are required:

```lua
{
    -- area to copy from minecraft window
    src = {
        x = 160,
        y = 900,
        w = 100,
        h = 100,
    },

    -- absolute location/size of mirror in waywall window
    dst = {
        x = 0,
        y = 300,
        w = 100,
        h = 100,
    },

    -- optional
    color_key = {
        input = "#dddddd",
        output = "#ee1111",
    },

    -- optional
    depth = 0,

    -- optional
    shader = "shader_name",
}
```

The `color_key` option allows you to perform color keying on the mirrored area,
which will only preserve pixels of the given `input` color and change them to
the `output` color.

For more information on custom shaders, see [Shaders].

## Mirror object

This function will return an object which can be used to remove the mirror from
the waywall window at a later point. The only method made available by the
returned mirror object is `close`.

If this object is not stored in a permanent location, the mirror may randomly
disappear when it gets garbage collected.

### Arguments

  - `options`: table

### Return values

  - `mirror`: table

> This function cannot be called during startup.

[Shaders]: 01_options_shaders.md
