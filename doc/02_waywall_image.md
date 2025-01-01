# image

This function loads a PNG image from the file system and displays it over top
of the waywall window. You may want to use it for e.g. your boat eye overlay.

The `options` table can have the following options, although only `dst` is
required:

```lua
{
    -- absolute location/size of image in waywall window
    dst = {
        x = 100,
        y = 100,
        w = 200,
        h = 200,
    },

    -- optional
    shader = "shader_name"
}
```

For more information on custom shaders, see [Shaders].

## Image object

This function will return an object which can be used to remove the image from
the waywall window at a later point. The only method made available by the
returned image object is `close`.

If this object is not stored in a permanent location, the image may randomly
disappear when it gets garbage collected.

### Arguments

  - `path`: string
  - `options`: table

### Return values

  - `image`: table

> This function cannot be called during startup.

[Shaders]: 01_options_shaders.md
