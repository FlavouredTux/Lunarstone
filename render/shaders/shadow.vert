#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 7) in vec2 inUv;
// Per caster: model matrix, and for cut-out casters their UV transform and colour map.
layout(location = 1) in mat4 inModel;
layout(location = 5) in vec4 inUvTransform;
layout(location = 6) in uint inTexture;

layout(location = 0) out vec2 outUv;
layout(location = 1) flat out uint outTexture;

// Into a face of a local light's cube map (lights.glsl's atlas) rather than
// the sun's map: the face's view-projection.
layout(constant_id = 4) const bool localShadow = false;
layout(push_constant) uniform FaceView
{
    mat4 viewProjection;
} face;

void main()
{
    gl_Position = (localShadow ? face.viewProjection : frame.shadowViewProjection) * (inModel * vec4(inPosition, 1.0));
    outUv = inUv * inUvTransform.xy + inUvTransform.zw;
    outTexture = inTexture;
}
