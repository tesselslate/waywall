precision highp float;

varying vec2 f_src_pos;
varying vec4 f_src_rgba;
varying vec4 f_dst_rgba;

uniform sampler2D u_texture;

const float threshold = 0.01;

void main() {
    vec4 color = texture2D(u_texture, f_src_pos);

    if (f_dst_rgba.a == 0.0) {
        gl_FragColor = color;
    } else {
        if (all(lessThan(abs(f_src_rgba.rgb - color.rgb), vec3(threshold)))) {
            gl_FragColor = f_dst_rgba;
        } else {
            gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        }
    }
}

// vim:ft=glsl
