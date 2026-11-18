#version 440

// The overlay's marker: a small quad around the instrument's tip. `corner` runs from -1 to 1
// across the quad; the matrix puts it where the tip is in the picture (and already includes the
// backend's clip-space correction).
layout(location = 0) in vec2 corner;

layout(location = 0) out vec2 vLocal;

layout(std140, binding = 0) uniform Hud {
    mat4 transform;
    vec4 colour;
    vec4 ring;  // x: inner radius, y: outer radius, z: edge softness; all in units of `corner`
} hud;

void main()
{
    vLocal = corner;
    gl_Position = hud.transform * vec4(corner, 0.0, 1.0);
}
