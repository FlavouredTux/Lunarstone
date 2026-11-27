#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "frame.glsl"
#include "sky.glsl"

// The decal layer: the decals on solid parts (DrawItem::onSolid), drawn over
// the depth pre-pass, lowest zIndex first, each one's colour over the ones
// before it, premultiplied. The opaque pass then puts it over the colour of
// the surface it shades and lights the two together (mesh.frag), instead of
// every decal being lit again on its own after it. The colour is the one
// mesh.frag gives a decal before lighting it: its texture tinted by Color3.

layout(location = 0) in vec4 inWorldUv;  // w: u
layout(location = 1) in vec4 inNormalUv; // w: v
layout(location = 2) in vec4 inColor;
layout(location = 6) in vec3 inVertexColor;
layout(location = 7) flat in uint inShade;

layout(location = 0) out vec4 outColor;

const uint flagClamp = 4u; // mesh.frag's

void main()
{
    Shade shade = shades[inShade];
    vec2 uv = vec2(inWorldUv.w, inNormalUv.w) * shade.uvTransform.xy + shade.uvTransform.zw;
    vec4 t = sampleTexture(shade.maps.x, uv, (shade.extra.z & flagClamp) != 0u ? 1u : 0u);
    float alpha = inColor.a * t.a;
    outColor = vec4(t.rgb * shade.tint.rgb * inVertexColor * alpha, alpha);
}
