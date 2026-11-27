#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

// render::EffectVertex, with its batch's look and texture filled in.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inUvq;  // u * q, v * q, q
layout(location = 2) in vec4 inColor; // sRGB colour, opacity
layout(location = 3) in vec3 inLook;  // one less LightEmission, LightInfluence, Brightness
layout(location = 4) in uint inTexture; // flags in the top bits (effect.frag)
layout(location = 5) in vec4 inExtra;   // second flipbook frame's uv, how far towards it; depth offset

layout(location = 0) out vec3 outWorld;
layout(location = 1) out vec3 outUvq;
layout(location = 2) out vec4 outColor; // linear
layout(location = 3) flat out vec3 outLook;
layout(location = 4) flat out uint outTexture;
layout(location = 5) out vec3 outFrame; // second flipbook frame's uv, how far towards it

void main()
{
    gl_Position = frame.viewProjection * vec4(inPosition, 1.0);
    // ZOffset, the client's way (ParticleLightingVS): depth tested where the
    // vertex would be moved that far towards the camera, drawn where it is.
    // Never past the near plane.
    if (inExtra.w != 0.0) {
        vec3 toCamera = frame.cameraPosition.xyz - inPosition;
        float distance = length(toCamera);
        float moved = min(inExtra.w, distance - 2.0 * frame.depthParams.x);
        vec4 shifted = frame.viewProjection * vec4(inPosition + toCamera / max(distance, 1e-4) * moved, 1.0);
        gl_Position.z = shifted.z / shifted.w * gl_Position.w;
    }
    outWorld = inPosition;
    outUvq = inUvq;
    vec3 c = clamp(inColor.rgb, 0.0, 1.0);
    outColor = vec4(mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)), inColor.a);
    outLook = inLook;
    outTexture = inTexture;
    outFrame = inExtra.xyz;
}
