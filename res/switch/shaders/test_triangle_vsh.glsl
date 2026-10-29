#version 460

// Phase 2 test triangle: 2D position + color per vertex

layout (location = 0) in vec2 inPos;
layout (location = 1) in vec4 inColor;

layout (location = 0) out vec4 outColor;

void main()
{
    gl_Position = vec4(inPos, 0.0, 1.0);
    outColor = inColor;
}
