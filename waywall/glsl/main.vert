attribute vec2 v_texcoord;
attribute vec2 v_pos;

uniform vec2 u_size;

varying vec2 f_texcoord;

void main() {
    gl_Position = vec4(v_pos.x, v_pos.y, 1.0, 1.0);

    f_texcoord = v_texcoord / u_size;
}

// vim:ft=glsl
