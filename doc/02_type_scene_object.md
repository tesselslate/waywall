# Scene objects

Scene objects represent various kinds of graphics which are drawn on the
waywall window. There are currently three kinds of scene objects:

  - [Images](02_type_image.md)
  - [Mirrors](02_type_mirror.md)
  - [Text](02_type_text.md)

All scene objects share a common set of methods, although some may have extra
methods of their own.

Scene objects will disappear either when they are explicitly [closed](#close) or
when they are garbage collected by the Lua virtual machine.

<div class="warning">

Because scene objects disappear when garbage collected, you should make sure to
store a reachable reference to any objects you create, such as in a local
variable at the top level of your configuration.

Additionally, reloading your configuration will cause all live scene objects to
be destroyed (since they are garbage collected when the Lua virtual machine is
destroyed and recreated for your new configuration.)

</div>

## Depth

All scene objects have a depth value, which can be set at the time of creation
or with the [`set_depth`](#set_depth) method. Scene objects with a greater depth
will appear in front of objects with a lesser depth. Scene objects with a
negative depth will appear beneath the Minecraft instance.

Scene objects without an explicitly specified depth, or objects whose depth has
been set to 0, follow different ordering rules. The complete order in which
scene objects appear is as follows (starting from the furthest forward to
the furthest back):

  - All objects with positive depth
  - Images with unspecified depth
  - Mirrors with unspecified depth
  - Text with unspecified depth
  - The Minecraft instance
  - All objects with negative depth

## Methods

### close

This method closes the scene object, causing it to disappear from the scene. It
is invalid to call any methods on a scene object after it has been closed.

#### Arguments

- None

#### Return values

- None

### get_depth

This method returns the current depth of the scene object, or 0 if no depth has
been set.

#### Arguments

- None

#### Return values

- `depth`: number

### set_depth

This method sets the depth of the scene object. After the depth is set, it will
be arranged according to the [ordering rules](#depth).

#### Arguments

- `depth`: number

#### Return values

- None
