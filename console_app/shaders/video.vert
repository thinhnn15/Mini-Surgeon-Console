#version 440

// A quad covering the viewport. Positions arrive already in the backend's clip space
// (corrected on the CPU once, see VideoRenderer::initialize), so no matrix is needed here.
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texCoord;

layout(location = 0) out vec2 vTexCoord;

void main()
{
    vTexCoord = texCoord;
    gl_Position = vec4(position, 0.0, 1.0);
}
