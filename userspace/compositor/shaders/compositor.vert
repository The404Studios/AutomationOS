// AutomationOS Compositor Vertex Shader
// OpenGL ES 2.0 / GLSL 120

#version 120

// Input attributes
attribute vec2 position;    // Vertex position (screen space)
attribute vec2 texcoord;    // Texture coordinates (0-1)

// Output to fragment shader
varying vec2 v_texcoord;

// Uniforms
uniform mat4 projection;    // Orthographic projection matrix

void main() {
    // Transform position to clip space
    gl_Position = projection * vec4(position, 0.0, 1.0);

    // Pass texture coordinates to fragment shader
    v_texcoord = texcoord;
}
