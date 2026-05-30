// AutomationOS Compositor Fragment Shader
// OpenGL ES 2.0 / GLSL 120

#version 120

// Input from vertex shader
varying vec2 v_texcoord;

// Uniforms
uniform sampler2D texture;      // Window texture
uniform float alpha;            // Global alpha (for animations)
uniform vec4 tint_color;        // Color tint (default: white)
uniform bool dim_unfocused;     // Dim unfocused windows?
uniform float dim_factor;       // Dim amount (0.0 - 1.0)

void main() {
    // Sample window texture
    vec4 color = texture2D(texture, v_texcoord);

    // Apply global alpha (for fade animations)
    color.a *= alpha;

    // Apply color tint
    color *= tint_color;

    // Apply dimming for unfocused windows
    if (dim_unfocused) {
        color.rgb *= (1.0 - dim_factor);
    }

    // Output final color
    gl_FragColor = color;
}
