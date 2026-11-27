#version 460

// Highlights' mask (HighlightPass): which highlight each pixel's item is
// under, drawn with mesh.vert.
layout(push_constant) uniform Push
{
    layout(offset = 64) uint id;
}
push;

// mesh.vert's outputs, unread: declared so nothing it writes goes unmatched.
layout(location = 0) in vec4 inWorld;
layout(location = 1) in vec4 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec3 inStuds;
layout(location = 4) in vec3 inLocalNormal;
layout(location = 5) in vec4 inTangent;
layout(location = 6) in vec3 inVertexColor;
layout(location = 7) flat in uint inShade;
layout(location = 8) in vec4 inMaterialWeightsA;
layout(location = 9) in vec4 inMaterialWeightsB;

layout(location = 0) out uint outId;

void main()
{
    // Never true; keeps the optimizer from dropping the flat input, which
    // the validation layers would then warn about.
    if (inShade == 0xffffffffu)
        discard;
    outId = push.id;
}
