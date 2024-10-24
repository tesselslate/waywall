precision highp float;

varying vec2 f_tex;
varying vec4 f_src_rgba;
varying vec4 f_dst_rgba;

uniform sampler2D u_texture;

const float threshold = 0.01;

void main() {
    vec3 color = texture2D(u_texture, f_tex).rgb;

    if (f_dst_rgba.a == 0.0) {
        gl_FragColor = vec4(color.rgb, 1.0);
    } else {
        if (all(lessThan(abs(f_src_rgba.rgb - color), vec3(threshold)))) {
            gl_FragColor = f_dst_rgba;
        } else {
            gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        }
    }
}

// vim:ft=glsl
