precision highp float;

varying vec2 f_texcoord;

uniform sampler2D f_texture;

void main() {
    vec3 color = texture2D(f_texture, f_texcoord).rgb;
    gl_FragColor = vec4(color.rgb, 1.0);
}

// vim:ft=glsl
