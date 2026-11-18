#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D videoFrame;

void main()
{
    // Video is opaque: ignore whatever the source put in alpha.
    fragColor = vec4(texture(videoFrame, vTexCoord).rgb, 1.0);
}
