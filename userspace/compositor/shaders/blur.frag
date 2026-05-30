// AutomationOS Gaussian Blur Shader
// Two-pass separable blur (horizontal + vertical)

#version 120

varying vec2 v_texcoord;

uniform sampler2D texture;
uniform vec2 direction;         // (1,0) for horizontal, (0,1) for vertical
uniform float blur_radius;      // Blur amount
uniform vec2 resolution;        // Texture resolution

// 5-tap Gaussian kernel weights (σ ≈ 1.0)
const float weights[5] = float[](
    0.227027,   // Center
    0.1945946,  // ±1
    0.1216216,  // ±2
    0.054054,   // ±3
    0.016216    // ±4
);

void main() {
    // Calculate texel size
    vec2 tex_offset = 1.0 / resolution;

    // Scale by blur radius
    tex_offset *= direction * blur_radius;

    // Accumulate color
    vec4 color = vec4(0.0);

    // Center sample
    color += texture2D(texture, v_texcoord) * weights[0];

    // Surrounding samples (both directions)
    for (int i = 1; i < 5; i++) {
        // Positive direction
        color += texture2D(texture, v_texcoord + tex_offset * float(i)) * weights[i];

        // Negative direction
        color += texture2D(texture, v_texcoord - tex_offset * float(i)) * weights[i];
    }

    gl_FragColor = color;
}
