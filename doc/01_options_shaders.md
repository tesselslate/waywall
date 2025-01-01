# Shaders

The `shaders` section of the configuration table allows you to load custom
OpenGL shaders to further customize the appearance of various scene objects
(mirrors, images, text).

<div class="warning">

This option is intended for more advanced users who are able to write shaders
to do what they want. A tutorial on how to write OpenGL shaders is outside of
the scope of this documentation (and I am not particularly qualified to write
one anyway.)

</div>

## Default values

```lua
local config = {
    shaders = {},
}

return config
```

## Configuration

The `shaders` table should contain a list of key-value pairs where each key
is a string containing the name of the shader and the value is a table
describing the shader's sources (fragment and/or vertex). For example:

```lua
local read_file = function(name)
    local home = os.getenv("HOME")

    local file = io.open(home .. "/.config/waywall/" .. name, "r")
    local data = file:read("*a")
    file:close()

    return data
end

local config = {
    shaders = {
        ["pie_chart"] = {
            vertex = read_file("pie_chart.vert"),
            fragment = read_file("pie_chart.frag"),
        }
    }
}

return config
```

If either `vertex` or `fragment` is unspecified, they will default to the
implementations provided by waywall's built-in "texcopy" shader. You can
view the sources for the texcopy shader [here](https://github.com/tesselslate/waywall/tree/main/waywall/glsl).

## Vertex format

The following attributes are provided to the vertex shader. You can use as many
as you like, and leaving attributes unused is fine.

### `v_src_pos` (vec2)

`v_src_pos` contains the pixel coordinate which should be sampled from the
source texture.

### `v_dst_pos` (vec2)

`v_dst_pos` contains the pixel coordinate at which the vertex is located.

### `v_src_rgba` (vec4)

`v_src_rgba` contains the source color for the color-keying operation (if any).
An alpha value of 0 signals that no color keying should happen.

### `v_dst_rgba` (vec4)

`v_dst_rgba` contains the output color for the color-keying operation (if any).

## Uniforms

The following uniforms are provided to the vertex shader:

### `u_src_size` (vec2)

`u_src_size` contains the size of the source texture in pixels.

### `u_dst_size` (vec2)

`u_dst_size` contains the size of the destination texture (waywall window) in
pixels.

## Example

The following shaders perform color-keying to only accept the three main colors
of the `gameRenderer` pie chart (orange for block entities, pink for entities,
and green for unspecified):

### `pie_chart.vert`

```c
attribute vec2 v_src_pos;
attribute vec2 v_dst_pos;

uniform vec2 u_src_size;
uniform vec2 u_dst_size;

varying vec2 f_src_pos;

void main() {
    gl_Position.x = 2.0 * (v_dst_pos.x / u_dst_size.x) - 1.0;
    gl_Position.y = 1.0 - 2.0 * (v_dst_pos.y / u_dst_size.y);
    gl_Position.zw = vec2(1.0);

    f_src_pos = v_src_pos / u_src_size;
}
```

### `pie_chart.frag`

```c
precision highp float;

varying vec2 f_src_pos;

uniform sampler2D u_texture;

const float threshold = 0.01;
const vec3 pink = vec3(0.882, 0.271, 0.761); // #e145c2
const vec3 orange = vec3(0.914, 0.427, 0.302); // #e96d4d
const vec3 green = vec3(0.271, 0.796, 0.396); // #45cb65

void main() {
    vec4 color = texture2D(u_texture, f_src_pos);

    bool is_pink = all(lessThan(abs(color.rgb - pink), vec3(threshold)));
    bool is_orange = all(lessThan(abs(color.rgb - orange), vec3(threshold)));
    bool is_green = all(lessThan(abs(color.rgb - green), vec3(threshold)));

    if (is_pink || is_orange || is_green) {
        gl_FragColor = color;
    } else {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
}
```
