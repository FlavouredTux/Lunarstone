#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "frame.glsl"

// Cut-out shadow casters: holes in the colour map let the sun through.
layout(location = 0) in vec2 inUv;
layout(location = 1) flat in uint inTexture;

void main()
{
    if (texture(sampler2D(textures[nonuniformEXT(inTexture)], samplers[0]), inUv).a < 0.5)
        discard;
}
