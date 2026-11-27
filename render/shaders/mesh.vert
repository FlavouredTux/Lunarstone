#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inVertexColor;
layout(location = 8) in vec2 inUv;
layout(location = 9) in vec4 inTangent; // object space, w = bitangent sign
layout(location = 10) in vec4 inMaterialWeightsA; // terrain: weights of palette entries 0-3...
layout(location = 11) in vec4 inMaterialWeightsB; // ...and 4-7
// Per instance: model matrix (locations 3-6) and linear RGBA colour. The
// rest of how it shades sits in the shade buffer, for the fragment shader:
// instance attributes are fetched per vertex, which adds up on big meshes.
layout(location = 3) in mat4 inModel;
layout(location = 7) in vec4 inColor;

// The depth pre-pass and the opaque pass after it compare depths for
// equality: both must come out the same to the bit.
invariant gl_Position;

layout(location = 0) out vec4 outWorld;  // w: u
layout(location = 1) out vec4 outNormal; // w: v
layout(location = 2) out vec4 outColor;
layout(location = 3) out vec3 outStuds;
layout(location = 4) out vec3 outLocalNormal;
layout(location = 5) out vec4 outTangent;
layout(location = 6) out vec3 outVertexColor;
layout(location = 7) flat out uint outShade;
layout(location = 8) out vec4 outMaterialWeightsA;
layout(location = 9) out vec4 outMaterialWeightsB;

// Terrain water's pipeline: the swell moves its vertices.
layout(constant_id = 2) const bool water = false;

void main()
{
    vec4 world = inModel * vec4(inPosition, 1.0);
    if (water && inNormal.y > 0.5) {
        // The client's swell (SmoothWaterVSUnified): two sine waves crossing
        // diagonally, phase and height from the water layer's blend.yz,
        // fading out with distance. Only open surfaces move, so shores stay
        // put. GUESS: the client's wave length and fade distance.
        TerrainLayer layer = frame.terrainLayers[1];
        float k = 0.1; // radians per stud
        float swell = sin((world.z - world.x) * k - layer.blend.y) + sin((world.z + world.x) * k + layer.blend.y);
        float fade = clamp(1.0 - distance(world.xyz, frame.cameraPosition.xyz) / 500.0, 0.0, 1.0);
        world.y += swell * layer.blend.z * fade;
    }
    gl_Position = frame.viewProjection * world;
    outWorld = vec4(world.xyz, inUv.x);

    // Models are rotation times scale, so the inverse transpose is the model
    // divided by the squared scale per axis.
    mat3 m = mat3(inModel);
    vec3 scale2 = max(vec3(dot(m[0], m[0]), dot(m[1], m[1]), dot(m[2], m[2])), vec3(1e-12));
    outNormal = vec4(m * (inNormal / scale2), inUv.y);
    outTangent = vec4(m * inTangent.xyz, inTangent.w);

    outColor = inColor * vec4(inVertexColor.rgb, 1.0);
    outVertexColor = inVertexColor.rgb;
    outShade = gl_InstanceIndex;
    // Object space in studs from the box's low corner, so material detail
    // keeps its size on any part and studs line up with its edges.
    outStuds = (inPosition + 0.5) * sqrt(scale2);
    outLocalNormal = inNormal;
    outMaterialWeightsA = inMaterialWeightsA;
    outMaterialWeightsB = inMaterialWeightsB;
}
