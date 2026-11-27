#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

// One triangle over the whole screen at the far plane (reversed Z: depth 0),
// carrying the view ray through each pixel.
layout(location = 0) out vec3 outRay;

void main()
{
    vec2 ndc = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    // Vulkan's y points down the screen.
    outRay = frame.skyForward.xyz + frame.skyRight.xyz * ndc.x - frame.skyUp.xyz * ndc.y;
}
