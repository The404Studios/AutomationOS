// AutomationOS Drop Shadow Shader

#version 120

varying vec2 v_texcoord;

uniform sampler2D texture;
uniform vec4 shadow_color;      // Shadow color (RGBA)
uniform float shadow_opacity;   // Shadow opacity multiplier

void main() {
    // Sample texture alpha channel
    vec4 texel = texture2D(texture, v_texcoord);

    // Use alpha as shadow intensity
    float shadow_intensity = texel.a * shadow_opacity;

    // Output shadow color with computed intensity
    gl_FragColor = vec4(shadow_color.rgb, shadow_intensity);
}
