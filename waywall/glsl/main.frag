precision highp float;

varying vec2 f_texcoord;

uniform sampler2D u_texture;
uniform vec4 u_colorkey_src;
uniform vec4 u_colorkey_dst;

const float threshold = 0.01;

void main() {
    vec3 color = texture2D(u_texture, f_texcoord).rgb;

    if (u_colorkey_dst.a == 0.0) {
        gl_FragColor = vec4(color.rgb, 1.0);
    } else {
        if (all(lessThan(abs(u_colorkey_src.rgb - color), vec3(threshold)))) {
            gl_FragColor = u_colorkey_dst;
        } else {
            gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        }
    }
}

// vim:ft=glsl
