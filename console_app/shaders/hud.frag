#version 440

layout(location = 0) in vec2 vLocal;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Hud {
    mat4 transform;
    vec4 colour;
    vec4 ring;  // x: inner radius, y: outer radius, z: edge softness; all in units of vLocal
} hud;

void main()
{
    // A ring: opaque between the two radii, fading out over `softness` on both edges (about
    // one pixel, set by the renderer), so the circle is smooth at any size.
    float d = length(vLocal);
    float inside = smoothstep(hud.ring.x - hud.ring.z, hud.ring.x, d);
    float outside = 1.0 - smoothstep(hud.ring.y, hud.ring.y + hud.ring.z, d);
    fragColor = vec4(hud.colour.rgb, hud.colour.a * inside * outside);
}
