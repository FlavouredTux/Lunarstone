#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "frame.glsl"
#include "sky.glsl"
#include "lights.glsl"
// The tone curve the rest of the scene went through (tone.glsl).
// GUESS: the client draws these into its HDR target before tone mapping.
#include "tone.glsl"

// Beams and trails (render::EffectGeometry), shaded as the client's trail
// shaders do (TrailFS, TrailFSLightUnified): the texture times the vertex
// colour; unlit that times Brightness, lit that times the ambient, the sky
// as far as it reaches and local lights (the light map's, capped with the
// ambient light) and the sun, with no normal to turn from it; mixed by
// LightInfluence. The client's particles (ParticleLightingFS) light the
// same way. Fog fades it out instead of tinting it, which keeps
// additive effects from glowing in the fog colour. The output is
// premultiplied, its alpha scaled by one less LightEmission: blended with
// ONE, ONE_MINUS_SRC_ALPHA that's normal blending at 0 and adding at 1.

layout(location = 0) in vec3 inWorld;
layout(location = 1) in vec3 inUvq;
layout(location = 2) in vec4 inColor;
layout(location = 3) flat in vec3 inLook;
layout(location = 4) flat in uint inTexture;
layout(location = 5) in vec3 inFrame; // second flipbook frame's uv, how far towards it

layout(location = 0) out vec4 outColor;

// Drawn through the scene's UNORM view: the colour is written sRGB-encoded
// and blended as it is, as in the client's gamma-space frame buffer.
layout(constant_id = 10) const bool gammaTarget = false;

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;
// The light grid, as mesh.frag reads it: red sky, green sun.
layout(set = 0, binding = 6) uniform sampler3D lightGrid;

// The light grid at a point (no normal to push it out along): full light
// outside it.
vec2 lightGridAt(vec3 p)
{
    if (frame.lightGridMin.w < 0.5)
        return vec2(1.0);
    if (any(lessThan(p, frame.lightGridMin.xyz)) || any(greaterThan(p, frame.lightGridMax.xyz)))
        return vec2(1.0);
    return sampleLightGrid(lightGrid, p);
}

// The sun's shadow map at a point, one tap (TrailFSLightUnified takes one
// too); lit without the map. Ray traced frames have no map: the grid's sun
// shadows are all they get.
float sunVisibility(vec3 p)
{
    if (frame.shadow.x < 0.5)
        return 1.0;
    vec4 s = frame.shadowViewProjection * vec4(p, 1.0);
    if (max(abs(s.x), abs(s.y)) >= 1.0 || s.z >= 1.0)
        return 1.0;
    return texture(shadowMap, vec3(s.xy * 0.5 + 0.5, s.z));
}

// Flags in the top bits of the texture index.
const uint clampBit = 0x80000000u;   // clamp at the edges rather than repeat (particles)
const uint untonedBit = 0x40000000u; // exposed and clamped, not tone mapped (particles)

void main()
{
    uint index = inTexture & ~(clampBit | untonedBit);
    uint samplerIndex = (inTexture & clampBit) != 0u ? 1u : 0u;
    vec4 t = index != 0u ? sampleTexture(index, inUvq.xy / inUvq.z, samplerIndex) : vec4(1.0);
    // Flipbook frames blended as the client's particle shader blends them:
    // premultiplied, then mixed.
    if (inFrame.z > 0.0 && index != 0u) {
        vec4 next = sampleTexture(index, inFrame.xy, samplerIndex);
        float a = mix(t.a, next.a, inFrame.z);
        t = vec4(a > 0.0 ? mix(t.rgb * t.a, next.rgb * next.a, inFrame.z) / a : t.rgb, a);
    }
    vec3 albedo = t.rgb * inColor.rgb;
    float alpha = t.a * inColor.a;
    // Nothing to add and nothing to cover: the blend would leave the pixel
    // exactly as it is (premultiplied, alpha scaled), so the rest is skipped.
    if (alpha <= 0.0)
        discard;

    vec3 color = albedo * inLook.z;
    if (inLook.y > 0.0) {
        vec2 grid = lightGridAt(inWorld);
        vec3 light = cappedAmbient(frame.ambient.rgb + frame.skyAmbient.rgb * grid.x, lightMapAt(inWorld))
                     + frame.sunColor.rgb * grid.y * sunVisibility(inWorld);
        color = mix(color, albedo * light, inLook.y);
    }
    // Exposed, tone mapped and colour corrected in sRGB-encoded values, the
    // curve's own output (tone.glsl), and only then to linear: through the
    // UNORM view not even that.
    vec3 encoded = (inTexture & untonedBit) != 0u ? srgbEncode(color * frame.ambient.w)
                                                   : toneCurveEncoded(color * frame.ambient.w, frame.tone.x > 0.5);

    vec3 toCamera = frame.cameraPosition.xyz - inWorld;
    float distance = length(toCamera);
    float fog = clamp((distance - frame.fogRange.x) / max(frame.fogRange.y - frame.fogRange.x, 1e-3), 0.0, 1.0);
    // GUESS: the Atmosphere's haze fades them the same way.
    float haze = 1.0 - exp(-distance * frame.atmosphereColor.w);
    alpha *= (1.0 - fog) * (1.0 - haze);

    encoded = colorCorrectEncoded(encoded);
    color = gammaTarget ? encoded : srgbDecode(encoded);
    outColor = vec4(color * alpha, alpha * inLook.x);
}
