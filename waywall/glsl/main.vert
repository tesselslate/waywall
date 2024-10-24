attribute vec2 v_pos;
attribute vec2 v_tex;
attribute vec4 v_src_rgba;
attribute vec4 v_dst_rgba;

uniform vec2 u_texsize;
uniform vec2 u_winsize;

varying vec2 f_tex;
varying vec4 f_src_rgba;
varying vec4 f_dst_rgba;

void main() {
    gl_Position.x = 2.0 * (v_pos.x / u_winsize.x) - 1.0;
    gl_Position.y = 1.0 - 2.0 * (v_pos.y / u_winsize.y);
    gl_Position.zw = vec2(1.0);

    f_tex = v_tex / u_texsize;
    f_src_rgba = v_src_rgba;
    f_dst_rgba = v_dst_rgba;
}

// vim:ft=glsl
