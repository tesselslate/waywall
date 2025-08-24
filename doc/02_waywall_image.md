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
    depth = 0,

    -- optional
    shader = "shader_name"
}
```

For more information on custom shaders, see [Shaders].

### Arguments

  - `path`: string
  - `options`: table

### Return values

  - `image`: [image object]

> This function cannot be called during startup.

[Shaders]: 01_options_shaders.md
[image object]: 02_type_image.md
