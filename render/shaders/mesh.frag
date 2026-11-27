#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#ifdef RAYTRACING
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif

#include "frame.glsl"
#include "sky.glsl"
#include "lights.glsl"
#include "tone.glsl"

// Cut-out pass: texture alpha below one half is a hole, the rest is opaque.
layout(constant_id = 0) const bool alphaTest = false;
// Terrain and water shading, only compiled into the pipelines that draw
// them: the parts' pipelines left without it run far more pixels at once
// (it held every pixel to the registers the terrain blend needs).
layout(constant_id = 3) const bool terrainItems = false;
// LUNAR_RT_SKIP: the ray traced features to leave out, as the bits of
// render::RayTracingSkip (sun 1, ao 2, lights 4, reflect 8, gather 16).
// Research only: at its default of 0 every ray below is traced, as it
// always was.
layout(constant_id = 5) const uint rtSkipMask = 0u;
// The opaque pass, which shades each pixel's front surface once after the
// depth pre-pass: the decals on it (the decal layer, decal_layer.frag) go
// into its colour before it's lit.
layout(constant_id = 6) const bool decalLayer = false;
layout(set = 0, binding = 14) uniform sampler2D decalLayerImage;

layout(location = 0) in vec4 inWorldUv;  // w: u
layout(location = 1) in vec4 inNormalUv; // w: v
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec3 inStuds; // object space studs from the low corner
layout(location = 4) in vec3 inLocalNormal;
layout(location = 5) in vec4 inTangent;
layout(location = 6) in vec3 inVertexColor;
layout(location = 7) flat in uint inShade;
layout(location = 8) in vec4 inMaterialWeightsA;
layout(location = 9) in vec4 inMaterialWeightsB;

layout(location = 0) out vec4 outColor;
#ifdef DENOISE
// The local lights' shared shadow ray, for the renderer's denoiser: how much
// this pixel's picture changes between that ray reaching its light and not
// (outRest), the same times what it found (outRestSeen), and the surface the
// denoiser keeps to (outGuide: geometric normal and view depth, which a
// cleared pixel's 0 says isn't a surface). Blended like the colour where decals
// draw them; rest zero where no ray was shared.
layout(location = 1) out vec4 outRest;
layout(location = 2) out vec4 outRestSeen;
layout(location = 3) out vec4 outGuide;
#endif

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;
#ifdef RAYTRACING
// Ray queries (render::RayScene): everything solid, and of each (instance
// custom index into rayInstances) its mesh and surface.
layout(set = 0, binding = 7) uniform accelerationStructureEXT rayScene;
const uint rayAlphaTested = 1u;
const uint rayTerrain = 2u;
// Ray masks: sun shadow rays see the shadow casters, reflections everything,
// local lights' shadow rays whatever casts shadows (with or without the sun's).
const uint maskCasts = 1u;
const uint maskReflected = 2u;
const uint maskLocalCasts = 4u;
const uint maskEncloses = 8u; // big enough to close a room in for the sky (skyRayOut)
// Which of this shader's rays LUNAR_RT_SKIP turns off (rtSkipMask's bits). A
// block left out takes the same value a ray that finds nothing would have.
const uint skipSun = 1u;
const uint skipOcclusion = 2u;
const uint skipLocalLights = 4u;
const uint skipReflections = 8u;
// The local lights' gather whole, not just its rays: with this bit the
// second loop gathers as it does without ray tracing, which is what says
// what the traced gather's bookkeeping costs over its rays.
const uint skipLightGather = 16u;
// (32 is the denoiser's, read on the CPU.)
const uint skipSkyReach = 64u;
struct RayInstance {
    uint64_t vertices; // RayVertices
    uint64_t indices;  // the full level's
    uint texture;
    uint flags;       // rayAlphaTested, rayTerrain
    vec4 uvTransform; // scale, offset
    vec4 color;       // linear; w: emissive
    uvec2 palette;    // terrain: material ids, a byte each
};
layout(set = 0, binding = 8, scalar) readonly buffer RayInstances
{
    RayInstance rayInstances[];
};
struct RayVertex { // the renderer's Vertex
    vec3 position;
    vec3 normal;
    vec2 uv;
    uint color;
    uint tangent;
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer RayVertices
{
    RayVertex v[];
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer RayIndices
{
    uint i[];
};

// Whether a candidate hit on a cut-out instance (only they are candidates)
// is solid: its texture's alpha at the hit, as the cut-out pass tests it.
bool raySolid(uint custom, int triangle, vec2 bary)
{
    RayInstance instance = rayInstances[custom];
    RayIndices indices = RayIndices(instance.indices);
    RayVertices vertices = RayVertices(instance.vertices);
    vec2 uv = vertices.v[indices.i[3 * triangle]].uv * (1.0 - bary.x - bary.y)
              + vertices.v[indices.i[3 * triangle + 1]].uv * bary.x
              + vertices.v[indices.i[3 * triangle + 2]].uv * bary.y;
    uv = uv * instance.uvTransform.xy + instance.uvTransform.zw;
    return textureLod(sampler2D(textures[nonuniformEXT(instance.texture)], samplers[0]), uv, 0.0).a >= 0.5;
}

// A stable jitter per pixel (interleaved gradient noise).
float pixelNoise(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// How much of the sun reaches this point, by a ray towards it: one ray a
// pixel, spread over a cone as wide as ShadowSoftness makes it, so the
// shadows soften away from their casters. Cut-out casters stop it where
// their texture is solid, as the cut-out pass draws them.
float rayTracedSun(vec3 world, vec3 n)
{
    vec3 l = frame.sunDirection.xyz;
    vec3 t = normalize(cross(l, abs(l.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(l, t);
    // GUESS: the cone, from a point sun to a few degrees across.
    float spread = 0.004 + 0.03 * frame.shadow.w;
    float angle = pixelNoise(gl_FragCoord.xy) * 6.2831853;
    float radius = sqrt(pixelNoise(gl_FragCoord.yx + 17.0)) * spread;
    vec3 direction = normalize(l + (t * cos(angle) + b * sin(angle)) * radius);
    // Off the surface a little, more the farther away, so it misses itself.
    vec3 origin = world + n * (0.02 + 0.0005 * length(world - frame.cameraPosition.xyz));

    // Leaves' own rays take the leaves around them as solid cards: in a
    // canopy the sun is behind some leaf either way, and alpha testing the
    // whole crown from every leaf was most of what the sun's rays cost.
    if ((rtSkipMask & skipSun) == 0u) {
        rayQueryEXT query;
        rayQueryInitializeEXT(query, rayScene,
                              gl_RayFlagsTerminateOnFirstHitEXT | (alphaTest ? gl_RayFlagsOpaqueEXT : 0u),
                              maskCasts, origin, 0.0, direction, 4096.0);
        while (rayQueryProceedEXT(query)) {
            if (raySolid(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false),
                         rayQueryGetIntersectionPrimitiveIndexEXT(query, false),
                         rayQueryGetIntersectionBarycentricsEXT(query, false)))
                rayQueryConfirmIntersectionEXT(query);
        }
        return rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
    }
    // LUNAR_RT_SKIP=sun: no ray, so the sun reaches everything: 1.0, what the
    // query returns where it meets nothing.
    return 1.0;
}

// --ambient-occlusion (Lunarstone's own; the client has none): how open
// the half of the world in front of a surface at world (geometric normal n)
// is, by rays spread over it like the light that falls on it, out to
// frame.occlusion.y studs. A ray stopped close by takes its share of light
// away, one stopped near its end hardly any; 1 is nothing around.
float tracedOcclusion(vec3 world, vec3 n)
{
    const int rays = 8;
    float reach = frame.occlusion.y;
    vec3 t = normalize(cross(n, abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, t);
    vec3 origin = world + n * (0.02 + 0.0005 * length(world - frame.cameraPosition.xyz));
    // A spiral of directions, turned and shifted per pixel.
    float spin = pixelNoise(gl_FragCoord.xy) * 6.2831853;
    float shift = pixelNoise(gl_FragCoord.yx + 7.0);
    float open = 0.0;
    if ((rtSkipMask & skipOcclusion) == 0u) {
        for (int i = 0; i < rays; ++i) {
            float u = (float(i) + shift) / float(rays);
            float r = sqrt(u);
            float angle = spin + float(i) * 2.3999632;
            vec3 d = normalize(t * (r * cos(angle)) + b * (r * sin(angle)) + n * sqrt(max(1.0 - u, 0.0)));
            rayQueryEXT query;
            rayQueryInitializeEXT(query, rayScene, alphaTest ? gl_RayFlagsOpaqueEXT : gl_RayFlagsNoneEXT, maskCasts,
                                  origin, 0.0, d, reach);
            while (rayQueryProceedEXT(query)) {
                if (raySolid(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false),
                             rayQueryGetIntersectionPrimitiveIndexEXT(query, false),
                             rayQueryGetIntersectionBarycentricsEXT(query, false)))
                    rayQueryConfirmIntersectionEXT(query);
            }
            float hit = rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT
                            ? reach
                            : rayQueryGetIntersectionTEXT(query, true);
            open += smoothstep(0.0, 1.0, hit / reach);
        }
        return open / float(rays);
    }
    // LUNAR_RT_SKIP=ao: no rays, so nothing around: 1.0, what the eight rays
    // all reaching their end add up to.
    return 1.0;
}

// Whether a local light distance away along l reaches a surface at world
// (geometric normal n): one ray, exact for a point. The ray skips the faces
// turned towards it, so it passes into a part and is stopped by the faces it
// would leave by: a light inside its part (a bulb, a lamp) still shines out,
// as the client's cube shadow maps, drawn from the light, see its part from
// inside. Closed parts in between still stop it. GUESS on the client's
// culling.
float localLightVisible(vec3 world, vec3 n, vec3 l, float distance)
{
    vec3 origin = world + n * (0.02 + 0.0005 * length(world - frame.cameraPosition.xyz));
    if ((rtSkipMask & skipLocalLights) == 0u) {
        rayQueryEXT query;
        rayQueryInitializeEXT(query, rayScene,
                              gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsCullFrontFacingTrianglesEXT
                                  | (alphaTest ? gl_RayFlagsOpaqueEXT : 0u),
                              maskLocalCasts, origin, 0.0, l, max(distance - 0.05, 0.0));
        while (rayQueryProceedEXT(query)) {
            if (raySolid(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false),
                         rayQueryGetIntersectionPrimitiveIndexEXT(query, false),
                         rayQueryGetIntersectionBarycentricsEXT(query, false)))
                rayQueryConfirmIntersectionEXT(query);
        }
        return rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
    }
    // LUNAR_RT_SKIP=lights: no ray, so the light reaches the surface: 1.0,
    // what the query returns where it meets nothing.
    return 1.0;
}
#endif

// The light grid (render::LightGrid): red how much sky, green how much sun
// reaches each 4-stud voxel. u is world x, v world z and w world y, wrapped.
layout(set = 0, binding = 6) uniform sampler3D lightGrid;

const uint flagClamp = 4u;
const uint flagPhysical = 8u;
const uint flagDecal = 16u;
const uint flagTintMask = 32u;
const uint flagTerrain = 64u;
const uint flagWater = 128u;
const float pi = 3.14159265;

float hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x),
               f.y);
}

// Distance to the nearest jittered point per cell: cobbles and pebbles.
float cells(vec2 p)
{
    vec2 cell = floor(p);
    vec2 offset = fract(p);
    float nearest = 1.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 n = vec2(x, y);
            vec2 point = n + vec2(hash(cell + n), hash(cell + n + 17.3));
            nearest = min(nearest, length(point - offset));
        }
    }
    return nearest;
}

// Albedo multiplier for the material's pattern, around 1.
float detail(int kind, vec2 p, float strength)
{
    float v = 0.5; // -> 1
    if (kind == 1) { // speckle
        v = noise(p) * 0.6 + noise(p * 2.7) * 0.4;
    } else if (kind == 2) { // grain, running along u
        v = noise(vec2(p.x * 0.4, p.y * 9.0)) * 0.7 + noise(p * 3.0) * 0.3;
    } else if (kind == 3) { // planks: grain broken by seams across v
        float seam = smoothstep(0.0, 0.04, abs(fract(p.y * 0.5) - 0.5) - 0.44);
        v = mix(0.0, noise(vec2(p.x * 0.4, p.y * 9.0)) * 0.6 + hash(vec2(floor(p.y * 0.5))) * 0.4, 1.0 - seam);
    } else if (kind == 4) { // bricks: courses offset by half a brick
        float course = floor(p.y * 1.6);
        float along = p.x * 0.8 + mod(course, 2.0) * 0.5;
        float mortar = max(step(0.45, abs(fract(along) - 0.5)), step(0.42, abs(fract(p.y * 1.6) - 0.5)));
        v = mix(0.35 + hash(vec2(floor(along), course)) * 0.4, 1.0, mortar);
    } else if (kind == 5) { // cells
        v = smoothstep(0.0, 0.5, cells(p));
    } else if (kind == 6) { // blades of grass
        v = noise(p) * 0.5 + noise(p * 3.1) * 0.5;
    } else if (kind == 7) { // sparkle
        v = 0.4 + step(0.9, noise(p * 3.0)) * 0.6;
    }
    return 1.0 + (v - 0.5) * 2.0 * strength;
}

// How much of the sun reaches this point: 3x3 filtered shadow map taps,
// fading out towards the edge of the area the map covers.
float sunVisibility(vec3 world, vec3 n)
{
#ifdef RAYTRACING
    if (frame.quality.y > 0.5)
        return rayTracedSun(world, n);
#endif
    if (frame.shadow.x < 0.5)
        return 1.0;
    vec4 p = frame.shadowViewProjection * vec4(world + n * frame.shadow.z, 1.0);
    vec2 uv = p.xy * 0.5 + 0.5;
    float edge = max(abs(p.x), abs(p.y));
    if (edge >= 1.0 || p.z >= 1.0)
        return 1.0;
    // ShadowSoftness spreads the taps: from one texel apart at 0 to four at 1.
    // GUESS: the client blurs its shadow map by it; the spread is ours.
    float spread = frame.shadow.y * (1.0 + 3.0 * frame.shadow.w);
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            lit += texture(shadowMap, vec3(uv + vec2(x, y) * spread, p.z));
    return mix(lit / 9.0, 1.0, smoothstep(0.8, 1.0, edge));
}

// The light grid where a surface is, the client's way (its vertex shaders):
// 6 studs out along the surface's normal, so a face looks at the air in
// front of it; full light outside the grid.
vec2 lightGridAt(vec3 world, vec3 n)
{
    if (frame.lightGridMin.w < 0.5)
        return vec2(1.0);
    vec3 p = world + n * 6.0;
    if (any(lessThan(p, frame.lightGridMin.xyz)) || any(greaterThan(p, frame.lightGridMax.xyz)))
        return vec2(1.0);
    return sampleLightGrid(lightGrid, p);
}

#ifdef RAYTRACING
// Whether a ray from origin along d meets the front of anything that can
// close a room in (maskEncloses: solid casters 4 studs across or more)
// within reach. Back faces let it through: a ray that starts inside a part
// (a crown of overlapping leaf blocks) gets out of it.
bool rayBlocked(vec3 origin, vec3 d, float reach)
{
    rayQueryEXT query;
    rayQueryInitializeEXT(query, rayScene,
                          gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsCullNoOpaqueEXT
                              | gl_RayFlagsCullBackFacingTrianglesEXT,
                          maskEncloses, origin, 0.0, d, reach);
    rayQueryProceedEXT(query);
    return rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// Ray traced, whether the sky can reach a surface at world (normal n) at
// all, its 2 x 2 pixel quad sharing the work: each pixel sends one ray out
// along one of four diagonals, 6 degrees up, and the quad has the sky
// where any of them gets 64 studs without meeting something that closes a
// room in (quadAny, in main): a window, a doorway, open ground, the edge of
// a canopy. A map's far boundary walls don't close it in, nor does a wall
// on one side or a roof overhead. The light grid carries the sky sideways
// into closed rooms (through the walls of rooms in a void, as the game builds
// them, and from the voxels above a thin ceiling that a face 6 studs out
// reads), which the client does too: there the room's faces take in the
// open sky. Ray traced, a closed room gets none; Lunarstone's own. GUESS:
// the reach and the angle, a room's size.
bool skyRayOut(vec3 world, vec3 n)
{
    if ((rtSkipMask & skipSkyReach) != 0u)
        return true;
    const vec3 directions[4] = vec3[](vec3(0.7036, 0.1, 0.7036), vec3(-0.7036, 0.1, 0.7036),
                                      vec3(0.7036, 0.1, -0.7036), vec3(-0.7036, 0.1, -0.7036));
    uint lane = (uint(gl_FragCoord.x) & 1u) + 2u * (uint(gl_FragCoord.y) & 1u);
    vec3 origin = world + n * (0.05 + 0.0005 * length(world - frame.cameraPosition.xyz));
    return !rayBlocked(origin, directions[lane], 64.0);
}
#endif

// --reflect-everything: how much of a surface is mirror, a third face on
// and all of it edge on; 0 without it, on cut-outs and on decals.
float everythingReflects(float ndv, bool decal)
{
    return frame.quality.w > 0.5 && !alphaTest && !decal ? 0.35 + 0.65 * pow(1.0 - ndv, 5.0) : 0.0;
}

// What reflects where the sky doesn't reach. The client blends towards a
// probe of the scene around the camera (PrefilteredEnvIndoorTexture) by
// one less the skylight. GUESS: without probes, surfaces lit by Ambient
// alone, of a typical albedo of a quarter.
vec3 indoorReflection()
{
    return frame.ambient.rgb * 0.25;
}

// Triplanar weights for an object-space normal: mostly the plane it faces.
vec3 triplanarWeights(vec3 n)
{
    vec3 w = pow(abs(n), vec3(4.0));
    return w / max(dot(w, vec3(1.0)), 1e-5);
}

// Whether c holds anywhere in this pixel's 2 x 2 quad (from the quad's
// differences, so in control flow the whole quad takes): a texture skipped
// on it is skipped by all four pixels or by none, and the samples taken keep
// their implicit derivatives, so their mip and anisotropy, as they were.
bool quadAny(bool c)
{
    float v = c ? 1.0 : 0.0;
    v = v + abs(dFdxFine(v)) > 0.0 ? 1.0 : 0.0;
    return v + abs(dFdyFine(v)) > 0.0;
}

// A box's faces, whose object-space normals lie along an axis, weigh one
// projection and nothing of the other two: those two aren't sampled (a
// sample times a weight of 0 added nothing), which is two thirds of the
// material's texture reads on most parts.
vec4 triplanarColor(uint index, vec3 p, vec3 w)
{
    vec4 c = vec4(0.0);
    if (quadAny(w.x > 0.0))
        c += sampleTexture(index, p.zy, 0u) * w.x;
    if (quadAny(w.y > 0.0))
        c += sampleTexture(index, p.xz, 0u) * w.y;
    if (quadAny(w.z > 0.0))
        c += sampleTexture(index, p.xy, 0u) * w.z;
    return c;
}

// A normal map projected three ways and blended ("whiteout"), in object
// space; as above, a projection without weight isn't sampled.
vec3 triplanarNormal(uint index, vec3 p, vec3 n, vec3 w)
{
    vec3 tx = quadAny(w.x > 0.0) ? sampleTexture(index, p.zy, 0u).xyz * 2.0 - 1.0 : vec3(0.0);
    vec3 ty = quadAny(w.y > 0.0) ? sampleTexture(index, p.xz, 0u).xyz * 2.0 - 1.0 : vec3(0.0);
    vec3 tz = quadAny(w.z > 0.0) ? sampleTexture(index, p.xy, 0u).xyz * 2.0 - 1.0 : vec3(0.0);
    vec3 s = sign(n);
    tx = vec3(tx.xy * vec2(s.x, 1.0) + n.zy, abs(tx.z) * n.x);
    ty = vec3(ty.xy * vec2(1.0, s.y) + n.xz, abs(ty.z) * n.y);
    tz = vec3(tz.xy * vec2(-s.z, 1.0) + n.xy, abs(tz.z) * n.z);
    return normalize(tx.zyx * w.x + ty.xzy * w.y + tz.xyz * w.z);
}

// The block face an object-space normal belongs to, as an Enum::NormalId,
// or 6 for curved and slanted surfaces.
uint blockFace(vec3 n)
{
    vec3 a = abs(n);
    if (a.x > 0.999)
        return n.x > 0.0 ? 0u : 3u;
    if (a.y > 0.999)
        return n.y > 0.0 ? 1u : 4u;
    if (a.z > 0.999)
        return n.z > 0.0 ? 2u : 5u;
    return 6u;
}

// Albedo multiplier for a stud (kind 1) or inlet (kind 2) at p, in studs
// across the face: a ring, lit from the top left.
float studShade(vec2 p, uint kind)
{
    vec2 d = fract(p) - 0.5;
    float r = length(d);
    float rim = smoothstep(0.24, 0.27, r) * (1.0 - smoothstep(0.31, 0.34, r));
    float inside = 1.0 - smoothstep(0.27, 0.3, r);
    float light = dot(d / max(r, 1e-4), vec2(-0.7071, -0.7071));
    float side = kind == 1u ? light : -light;
    return 1.0 + rim * (0.18 * side - 0.12) + inside * 0.04 * (kind == 1u ? 1.0 : -1.0);
}

// The client's ambient cube: the skybox's light from around n
// (EnvironmentDiffuseScale), each axis weighted by n squared. It's the
// outdoor half, scaled by the skylight; the client's indoor half comes from
// its scene probe, which isn't here (GUESS: nothing).
vec3 ambientCube(vec3 n)
{
    vec3 w = n * n;
    return w.x * (n.x > 0.0 ? frame.ambientCube[0].rgb : frame.ambientCube[1].rgb)
           + w.y * (n.y > 0.0 ? frame.ambientCube[2].rgb : frame.ambientCube[3].rgb)
           + w.z * (n.z > 0.0 ? frame.ambientCube[4].rgb : frame.ambientCube[5].rgb);
}

// The skybox as reflections show it along r: the skybox itself, or with
// the sharp reflections off (GraphicsSettings) its blurred average.
vec3 reflectedSky(vec3 r)
{
    return frame.quality.x > 0.5 ? skyboxColor(r) : skyboxBlurred(r);
}

// The skybox reflected along r (EnvironmentSpecularScale), tinted by the sky
// gradient the way the client does: bottom below the horizon, top from
// about 40 degrees up, and blended towards what's reflected indoors by one
// less the skylight. Rough surfaces see the blurred sky; the client picks a
// mip of its prefiltered map instead. sharp is skyboxColor(r), which the
// fully rough (roughness 1) don't use.
vec3 environmentReflection(vec3 sharp, vec3 r, float roughness, float skylight)
{
    float scale = frame.skyGradientBottom.w;
    if (scale <= 0.0)
        return vec3(0.0);
    vec3 gradient = mix(frame.skyGradientBottom.rgb, frame.skyGradientTop.rgb, clamp(r.y * 1.5882353, 0.0, 1.0));
    vec3 sky = roughness >= 1.0 ? skyboxBlurred(r) : mix(sharp, skyboxBlurred(r), roughness);
    return mix(indoorReflection(), sky * gradient, skylight) * scale;
}

// The client's sun highlight (DefaultUnifiedPlasticFS): a GGX-like lobe
// with Schlick's Fresnel off f0, for roughness 0.089 to 1.
vec3 sunHighlight(vec3 n, vec3 v, vec3 l, float roughness, vec3 f0)
{
    vec3 h = normalize(v + l);
    float ndh = max(dot(n, h), 0.001);
    float ldh = dot(l, h);
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (ndh * a2 - ndh) * ndh + 1.0;
    float f2 = (1.0 - ldh) * (1.0 - ldh);
    float f = f2 * f2 * (1.0 - ldh); // pow(1 - ldh, 5): ldh is never negative
    vec3 fresnel = vec3(f) + f0 * (1.0 - f);
    return fresnel * ((a2 + a2 * a2) / (d * d * (ldh * 3.0 + 0.5) * (ndh * 0.75 + 0.25)));
}

// The light a surface facing n gets, before its albedo, as the client's
// unified shaders add it up: Ambient, plus OutdoorAmbient's share and the
// skybox's light as far as the sky reaches (grid.x), local lights (local:
// the light map's, capped with the ambient light the client's way, or
// per pixel), the sun where neither the shadow map nor the grid (grid.y)
// shadows it, and the back light (ColorShift_Bottom's lamp, which the grid's
// sun scales too) on faces turned from it. specularF is the Fresnel the
// environment reflection takes away from the diffuse.
// How much of the light that comes from all around reaches this pixel
// (tracedOcclusion); 1 without --ambient-occlusion.
float occlusion = 1.0;

vec3 diffuseLight(vec3 n, float ndl, float sun, float specularF, vec2 grid, vec3 local)
{
    float keep = 1.0 - specularF * frame.skyGradientBottom.w;
    vec3 ambient = frame.ambient.rgb + frame.skyAmbient.rgb * grid.x;
    // Per-pixel lights add to the ambient light uncapped (the client's
    // Future shaders), the light map's alone is capped with it. Occlusion
    // takes from the light from all around, not from the per-pixel lights.
    ambient = frame.localLights.y > 0.5 ? ambient * occlusion + local : cappedAmbient(ambient, local) * occlusion;
    return ambientCube(n) * keep * grid.x * occlusion + ambient
           + frame.sunColor.rgb * max(ndl, 0.0) * sun * grid.y * keep + frame.backColor.rgb * max(-ndl, 0.0) * grid.y;
}

#ifdef DENOISE
// The light the rest of the traced lights add to the picture (diffuse
// times the surface's albedo, and specular), unshadowed, and what their
// shared ray found: -1 when there was none. Left for main to hand the
// denoiser.
vec3 restLight = vec3(0.0);
float restLightSeen = -1.0;
#endif

// Local lights each pixel near the camera sees (the cluster it's in), for a
// surface at world with geometric normal gn and shading normal n, seen
// along v: the light they add before the surface's albedo (diffuse) and
// their highlights (returned; the client's lobe for roughness and f0).
// Ray traced, each light's two parts go into one sum as it's met instead,
// its diffuse light times diffuseAlbedo (what the shading multiplies the
// light from all around by), and diffuse is left 0: what the shading does
// with them after is linear in them, and the one sum keeps the traced
// gather's loop in fewer registers. Without ray tracing the loop holds
// less, and two sums keep it in fewer than one and the albedo would.
// Ray traced, the three strongest lights each get a shadow ray, and the rest
// share one, sent to one of them picked by how strong it is (so on average
// each is shadowed right): the noise is left to the weakest light.
#ifdef RAYTRACING
// A light's strength, direction and distance and what it adds to the
// picture, unshadowed, as the gather's loop has them (for the few it goes
// back to after it).
void localLightAt(uint i, uvec2 cluster, vec3 world, vec3 gn, vec3 n, vec3 v, float roughness, vec3 f0,
                  vec3 diffuseAlbedo, out vec3 l, out float d, out vec3 added)
{
    LocalLight light = localLights[lightClusters[cluster.x + i]];
    float s = localLightStrength(light, world, n, l, d);
    if (light.flags.y >= 0.0)
        s *= localShadowMapped(light, world, gn, d);
    added = light.colorStart.rgb * s * (diffuseAlbedo + sunHighlight(n, v, l, roughness, f0));
}

// An independent number in [0, 1) for this pixel and light: what decides,
// light by light, which of the rest gets their shared ray.
float lightNoise(uint i)
{
    uvec3 h = uvec3(uvec2(gl_FragCoord.xy), i) * 1664525u + 1013904223u;
    h.x += h.y * h.z;
    h.y += h.z * h.x;
    h.z += h.x * h.y;
    h ^= h >> 16u;
    h.x += h.y * h.z;
    return float(h.x >> 8u) / 16777216.0;
}
#endif

vec3 localLightsPerPixel(vec3 world, vec3 gn, vec3 n, vec3 v, float roughness, vec3 f0, vec3 diffuseAlbedo,
                         out vec3 diffuse)
{
    vec3 added = vec3(0.0);
    diffuse = vec3(0.0);
    uvec2 cluster = lightCluster(gl_FragCoord.xy, 1.0 / gl_FragCoord.w);
    if (cluster.y == 0u)
        return added;
#ifdef RAYTRACING
    const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
    bool traced = frame.lightMap.z > 0.5 && (rtSkipMask & skipLightGather) == 0u;
    // One pass over the cluster's lights, as without ray tracing: they were
    // first walked once to find the strongest few and again to shade them,
    // and in a scene lit by many (the game's rooms) the walk is most of the
    // frame. The traced ones' light adds up unshadowed while the three
    // strongest so far are kept (cluster positions and weights, strongest
    // first, in scalars: the compiler didn't keep arrays shifted by an index
    // in registers); a light that misses them, or that a stronger one pushes
    // out, joins the rest, of which one is picked as it goes by how strong
    // it is (a weighted reservoir: each ends up picked as often as its share
    // of their weight). Only those four are looked at again after the loop.
    // A fourth light with a ray of its own cost the game 0.6 ms a frame at
    // 1440p and changed 0.14% of its pixels by more than 4/255.
    uint top0 = ~0u, top1 = ~0u, top2 = ~0u;
    float weight0 = 0.0, weight1 = 0.0, weight2 = 0.0;
    vec3 tracedAdded = vec3(0.0);
    uint picked = ~0u;
    float restWeight = 0.0;
#endif
    for (uint i = 0u; i < cluster.y; ++i) {
        uint index = lightClusters[cluster.x + i];
        // A cluster's lights reach some of its pixels, often not this one:
        // those are left before the other five-sixths of them are read.
        if (!localLightReaches(index, world))
            continue;
        LocalLight light = localLights[index];
        vec3 l;
        float d;
        float s = localLightStrength(light, world, n, l, d);
        if (s <= 0.0)
            continue;
        if (light.flags.y >= 0.0)
            s *= localShadowMapped(light, world, gn, d);
#ifdef RAYTRACING
        vec3 c = light.colorStart.rgb * s * (diffuseAlbedo + sunHighlight(n, v, l, roughness, f0));
        if (traced && light.flags.x > 0.5) {
            tracedAdded += c;
            float w = s * dot(light.colorStart.rgb, luma);
            // Into its place among the three: last if it beats the third,
            // then up past each weaker one, so it lands after any as strong.
            // What that pushes out, or the light itself, joins the rest.
            uint dropped = i;
            float droppedWeight = w;
            if (w > weight2) {
                dropped = top2;
                droppedWeight = weight2;
                weight2 = w;
                top2 = i;
                if (weight2 > weight1) {
                    weight2 = weight1;
                    top2 = top1;
                    weight1 = w;
                    top1 = i;
                    if (weight1 > weight0) {
                        weight1 = weight0;
                        top1 = top0;
                        weight0 = w;
                        top0 = i;
                    }
                }
            }
            if (droppedWeight > 0.0) {
                restWeight += droppedWeight;
                if (lightNoise(dropped) * restWeight < droppedWeight)
                    picked = dropped;
            }
            continue;
        }
        added += c;
#else
        vec3 c = light.colorStart.rgb * s;
        diffuse += c;
        added += sunHighlight(n, v, l, roughness, f0) * c;
#endif
    }
#ifdef RAYTRACING
    if (!traced)
        return added;
    // The three strongest, each shadowed by a ray of its own; the rest by the
    // ray to the one picked. What the three add comes off the rest's share.
    vec3 rest = tracedAdded;
    uint tops[3] = uint[3](top0, top1, top2);
    for (int k = 0; k < 3; ++k) {
        if (tops[k] == ~0u)
            continue;
        vec3 l, top;
        float d;
        localLightAt(tops[k], cluster, world, gn, n, v, roughness, f0, diffuseAlbedo, l, d, top);
        rest -= top;
        added += top * localLightVisible(world, gn, l, d);
    }
    if (picked != ~0u) {
        vec3 l, unused;
        float d;
        localLightAt(picked, cluster, world, gn, n, v, roughness, f0, diffuseAlbedo, l, d, unused);
        float seen = localLightVisible(world, gn, l, d);
        rest = max(rest, vec3(0.0));
        added += rest * seen;
#ifdef DENOISE
        restLight = rest;
        restLightSeen = seen;
#endif
    }
#endif
    return added;
}

#ifdef RAYTRACING
// What a reflection ray from a surface at world (geometric normal n) along
// r meets, lit, or false where it meets nothing and the sky shows. Hits are
// lit the light grid's way, whose green has the sun's shadows at 4 studs,
// without a second ray for the sun; textures show at a mip that coarsens
// with distance, terrain as its strongest material.
bool tracedReflection(vec3 world, vec3 n, vec3 r, out vec3 color)
{
    color = vec3(0.0);
    float distance = length(world - frame.cameraPosition.xyz);
    vec3 origin = world + n * (0.02 + 0.0005 * distance);
    // Bent normals can reflect into the surface: skim along it instead.
    r = normalize(r - n * min(dot(r, n) - 0.01, 0.0));
    if ((rtSkipMask & skipReflections) == 0u) {
        rayQueryEXT query;
        rayQueryInitializeEXT(query, rayScene, gl_RayFlagsNoneEXT, maskReflected, origin, 0.0, r, 4096.0);
        while (rayQueryProceedEXT(query)) {
            if (raySolid(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false),
                         rayQueryGetIntersectionPrimitiveIndexEXT(query, false),
                         rayQueryGetIntersectionBarycentricsEXT(query, false)))
                rayQueryConfirmIntersectionEXT(query);
        }
        if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT)
            return false;

        RayInstance instance = rayInstances[rayQueryGetIntersectionInstanceCustomIndexEXT(query, true)];
        int triangle = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
        vec2 bary = rayQueryGetIntersectionBarycentricsEXT(query, true);
        float t = rayQueryGetIntersectionTEXT(query, true);
        mat3 worldToObject = mat3(rayQueryGetIntersectionWorldToObjectEXT(query, true));
        RayIndices indices = RayIndices(instance.indices);
        RayVertices vertices = RayVertices(instance.vertices);
        RayVertex a = vertices.v[indices.i[3 * triangle]];
        RayVertex b = vertices.v[indices.i[3 * triangle + 1]];
        RayVertex c = vertices.v[indices.i[3 * triangle + 2]];
        vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
        vec3 normal = normalize(transpose(worldToObject) * (a.normal * w.x + b.normal * w.y + c.normal * w.z));
        if (dot(normal, r) > 0.0)
            normal = -normal;
        vec3 hit = origin + r * t;
        // GUESS: a mip a doubling of the ray's length past 8 studs.
        float lod = max(log2((distance + t) / 8.0), 0.0);

        vec3 tint = unpackUnorm4x8(a.color).rgb * w.x + unpackUnorm4x8(b.color).rgb * w.y
                    + unpackUnorm4x8(c.color).rgb * w.z;
        vec3 albedo = instance.color.rgb * tint;
        if ((instance.flags & rayTerrain) != 0u) {
            // The eight material weights are in the UVs' bits, a byte each.
            vec4 lo = unpackUnorm4x8(floatBitsToUint(a.uv.x)) * w.x + unpackUnorm4x8(floatBitsToUint(b.uv.x)) * w.y
                      + unpackUnorm4x8(floatBitsToUint(c.uv.x)) * w.z;
            vec4 hi = unpackUnorm4x8(floatBitsToUint(a.uv.y)) * w.x + unpackUnorm4x8(floatBitsToUint(b.uv.y)) * w.y
                      + unpackUnorm4x8(floatBitsToUint(c.uv.y)) * w.z;
            float weights[8] = float[8](lo.x, lo.y, lo.z, lo.w, hi.x, hi.y, hi.z, hi.w);
            int strongest = 0;
            for (int i = 1; i < 8; ++i)
                if (weights[i] > weights[strongest])
                    strongest = i;
            uint id = (instance.palette[strongest >> 2] >> (8u * uint(strongest & 3))) & 255u;
            TerrainLayer layer = frame.terrainLayers[min(id, 22u)];
            albedo = layer.color.rgb;
            if (layer.maps.x != 0u) {
                // Projected along the axis the surface faces most.
                vec3 facing = abs(normal);
                vec2 uv = facing.y > max(facing.x, facing.z) ? hit.xz * layer.tiling.x
                                                             : (facing.x > facing.z ? hit.zy : hit.xy) * layer.tiling.y;
                vec4 texel = textureLod(sampler2D(textures[nonuniformEXT(layer.maps.x)], samplers[0]), uv, lod);
                albedo = layer.color.w > 0.5 ? texel.rgb * mix(vec3(1.0), layer.color.rgb, texel.a)
                                             : texel.rgb * layer.color.rgb;
            }
        } else if (instance.texture != 0u) {
            vec2 uv = (a.uv * w.x + b.uv * w.y + c.uv * w.z) * instance.uvTransform.xy + instance.uvTransform.zw;
            vec4 texel = textureLod(sampler2D(textures[nonuniformEXT(instance.texture)], samplers[0]), uv, lod);
            albedo = (instance.flags & rayAlphaTested) != 0u ? texel.rgb * tint
                                                                : mix(albedo, texel.rgb * tint, texel.a);
        }

        vec2 grid = lightGridAt(hit, normal);
        // Local lights by the light map alone, capped the grid shaders' way.
        vec3 local = lightMapAt(hit + normal * 6.0);
        vec3 ambient = cappedAmbient(frame.ambient.rgb + frame.skyAmbient.rgb * grid.x, local);
        float hitNdl = dot(normal, frame.sunDirection.xyz);
        color = albedo
                * (ambientCube(normal) * grid.x + ambient + frame.sunColor.rgb * max(hitNdl, 0.0) * grid.y
                   + frame.backColor.rgb * max(-hitNdl, 0.0) * grid.y);
        color = mix(color, albedo * 1.5, instance.color.w);
        // The air between fades it, as it fades what the camera sees.
        color = mix(color, distanceColor(r), 1.0 - exp(-t * frame.atmosphereColor.w));
        return true;
    }
    // LUNAR_RT_SKIP=reflect: no ray, and false, what a ray that meets nothing
    // returns, so the sky shows where the reflection would have been.
    return false;
}
#endif

// How much of a surface's colour its reflection has to be before a ray is
// spent on it: below this the sky in its place doesn't show. GUESS.
const float tracedReflectionShare = 0.08;
// Rougher surfaces than this keep the sky: they'd blur what a ray finds
// most of the way into it anyway. GUESS.
const float tracedReflectionRoughness = 0.3;

// What reflects along r off a surface at world (geometric normal n): the
// sky (environmentReflection), or with ray traced reflections whatever a
// ray along r meets, blurred towards the sky's average by roughness, where
// the reflection is share of the colour or more.
vec3 sceneReflection(vec3 world, vec3 n, vec3 sharp, vec3 r, float roughness, float skylight, float share)
{
#ifdef RAYTRACING
    vec3 scene;
    // Not off cut-outs: leaves and fences, edge on and rippled by their
    // normal maps, would spend rays all over for glints nobody sees.
    // (With everything reflecting, main traces the one ray instead.)
    if (frame.quality.z > 0.5 && frame.quality.w < 0.5 && !alphaTest && share >= tracedReflectionShare
        && roughness < tracedReflectionRoughness
        && frame.skyGradientBottom.w > 0.0 && tracedReflection(world, n, r, scene))
        return mix(scene * frame.skyGradientBottom.w, environmentReflection(vec3(0.0), r, 1.0, skylight), roughness);
#endif
    return environmentReflection(sharp, r, roughness, skylight);
}

// What physicalLight multiplies the light from all around by: the share of
// the albedo the sun's Fresnel term leaves to diffuse light.
vec3 physicalDiffuse(vec3 albedo, float metalness, vec3 n, vec3 v, vec3 l)
{
    vec3 f0 = mix(vec3(0.04), albedo, metalness);
    vec3 h = normalize(l + v);
    vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - max(dot(v, h), 0.0), 5.0);
    return (1.0 - fresnel) * (1.0 - metalness) * albedo;
}

// GGX specular over Lambert diffuse, for one directional light.
vec3 physicalLight(vec3 world, vec3 geometric, vec3 albedo, float roughness, float metalness, vec3 n, vec3 v, vec3 l,
                   vec3 radiance, vec3 ambientLight, float skylight)
{
    vec3 f0 = mix(vec3(0.04), albedo, metalness);
    float ndl = max(dot(n, l), 0.0);
    float ndv = max(dot(n, v), 1e-4);
    vec3 h = normalize(l + v);
    float ndh = max(dot(n, h), 0.0);
    float a = max(roughness * roughness, 0.002);
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    float distribution = a2 / (pi * d * d);
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float visibility = 1.0 / ((ndv * (1.0 - k) + k) * (ndl * (1.0 - k) + k) * 4.0);
    vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - max(dot(v, h), 0.0), 5.0);
    vec3 specular = distribution * visibility * fresnel;
    vec3 diffuse = (1.0 - fresnel) * (1.0 - metalness) * albedo;
    // Light intensities here are what a white Lambert surface shows, so the
    // specular lobe (a radiance per irradiance / pi) gets pi back.
    vec3 color = diffuse * (ambientLight + radiance * ndl) + specular * radiance * ndl * pi;
    // Ambient reflection: the skybox by EnvironmentSpecularScale, as in the
    // client (nothing at 0, where metals go dark there too).
    vec3 fresnelAmbient = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - ndv, 5.0);
    vec3 r = reflect(-v, n);
    float share = max(fresnelAmbient.r, max(fresnelAmbient.g, fresnelAmbient.b)) * (1.0 - roughness);
    return color + fresnelAmbient * sceneReflection(world, geometric, reflectedSky(r), r, roughness, skylight, share);
}

vec3 rotate(vec4 q, vec3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// Triplanar sampling with its own scale per projection: tops (Y) tile by
// scale.x, sides by scale.y.
vec4 terrainColor(uint index, vec3 p, vec3 w, vec2 scale)
{
    return sampleTexture(index, p.zy * scale.y, 0u) * w.x + sampleTexture(index, p.xz * scale.x, 0u) * w.y
           + sampleTexture(index, p.xy * scale.y, 0u) * w.z;
}

vec3 terrainNormal(uint index, vec3 p, vec3 n, vec3 w, vec2 scale)
{
    vec3 tx = sampleTexture(index, p.zy * scale.y, 0u).xyz * 2.0 - 1.0;
    vec3 ty = sampleTexture(index, p.xz * scale.x, 0u).xyz * 2.0 - 1.0;
    vec3 tz = sampleTexture(index, p.xy * scale.y, 0u).xyz * 2.0 - 1.0;
    vec3 s = sign(n);
    tx = vec3(tx.xy * vec2(s.x, 1.0) + n.zy, abs(tx.z) * n.x);
    ty = vec3(ty.xy * vec2(1.0, s.y) + n.xz, abs(ty.z) * n.y);
    tz = vec3(tz.xy * vec2(-s.z, 1.0) + n.xy, abs(tz.z) * n.z);
    return normalize(tx.zyx * w.x + ty.xzy * w.y + tz.xyz * w.z);
}

float valueNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n = dot(i, vec3(1.0, 57.0, 113.0));
    vec4 a = fract(sin(vec4(n, n + 1.0, n + 57.0, n + 58.0)) * 43758.5453);
    vec4 b = fract(sin(vec4(n + 113.0, n + 114.0, n + 170.0, n + 171.0)) * 43758.5453);
    vec4 m = mix(a, b, f.z);
    vec2 r = mix(m.xy, m.zw, f.y);
    return mix(r.x, r.y, f.x);
}

// Terrain: up to eight materials blended by the vertex weights, sharpened
// by each material's height the way the client's terrain shader does it
// (SmoothCluster fragment shader in shaders_glsl3.pack): every material's
// height h becomes clamp(h * scale + offset) from its JSON blend min/max,
// times its weight, and a material keeps
// clamp((that - the largest other + width / 2) / width) of the surface,
// width being the weighted blend scales. Heights are the colour maps'
// brightness: the part TexturePacks used here have no height channel.
void terrainSurface(uvec4 palette, vec3 world, vec3 n, inout vec3 albedo, inout vec3 normal, out float emissive)
{
    float weights[8] = float[8](inMaterialWeightsA.x, inMaterialWeightsA.y, inMaterialWeightsA.z, inMaterialWeightsA.w,
                                inMaterialWeightsB.x, inMaterialWeightsB.y, inMaterialWeightsB.z, inMaterialWeightsB.w);
    vec3 tw = triplanarWeights(n);
    vec3 colors[8];
    float heights[8];
    float sum = 0.0;
    for (int i = 0; i < 8; ++i) {
        colors[i] = vec3(0.0);
        heights[i] = 0.0;
        if (weights[i] < 0.5 / 255.0) {
            weights[i] = 0.0;
            continue;
        }
        uint id = (palette[i >> 2] >> (8u * uint(i & 3))) & 255u;
        TerrainLayer layer = frame.terrainLayers[min(id, 22u)];
        vec3 tint = layer.color.rgb;
        if (layer.maps.x != 0u) {
            vec4 t = terrainColor(layer.maps.x, world, tw, layer.tiling.xy);
            colors[i] = layer.color.w > 0.5 ? t.rgb * mix(vec3(1.0), tint, t.a) : t.rgb * tint;
            heights[i] = dot(t.rgb, vec3(0.299, 0.587, 0.114));
        } else {
            colors[i] = tint;
            heights[i] = 0.5;
        }
        // materialTransitions.json: dominant materials spread over their
        // neighbours, and noise breaks up the edge.
        float noise = valueNoise(world * layer.blend.w) - 0.5;
        weights[i] = max(weights[i] * (1.0 + layer.tiling.z) + noise * layer.tiling.w * weights[i] * (1.0 - weights[i]) * 4.0, 0.0);
        sum += weights[i];
    }
    if (sum <= 0.0)
        return;
    float width = 0.0;
    float sharp[8];
    for (int i = 0; i < 8; ++i) {
        weights[i] /= sum;
        uint id = (palette[i >> 2] >> (8u * uint(i & 3))) & 255u;
        TerrainLayer layer = frame.terrainLayers[min(id, 22u)];
        sharp[i] = weights[i] * clamp(heights[i] * layer.blend.x + layer.blend.y, 0.0, 1.0);
        width += weights[i] * layer.blend.z;
    }
    float total = 0.0;
    float blended[8];
    for (int i = 0; i < 8; ++i) {
        blended[i] = 0.0;
        if (weights[i] <= 0.0)
            continue;
        float others = 0.0;
        for (int j = 0; j < 8; ++j)
            if (j != i)
                others = max(others, sharp[j]);
        blended[i] = width > 1e-4 ? clamp((sharp[i] - others + 0.5 * width) / width, 0.0, 1.0) : weights[i];
        total += blended[i];
    }
    if (total <= 1e-5) {
        for (int i = 0; i < 8; ++i)
            blended[i] = weights[i];
        total = 1.0;
    }
    vec3 color = vec3(0.0);
    vec3 bent = vec3(0.0);
    emissive = 0.0;
    for (int i = 0; i < 8; ++i) {
        float w = blended[i] / total;
        if (w <= 0.0)
            continue;
        uint id = (palette[i >> 2] >> (8u * uint(i & 3))) & 255u;
        TerrainLayer layer = frame.terrainLayers[min(id, 22u)];
        color += colors[i] * w;
        if (layer.maps.y != 0u)
            bent += terrainNormal(layer.maps.y, world, n, tw, layer.tiling.xy) * w;
        else
            bent += n * w;
    }
    albedo = color;
    normal = normalize(bent);
}

// What the camera sees behind the water: the scene's depth before any water
// was drawn (binding 5), turned back into studs along the view direction.
layout(set = 0, binding = 5) uniform sampler2D sceneDepth;

float viewDepth(float d)
{
    float near = frame.depthParams.x, far = frame.depthParams.y;
    return near * far / (d * (far - near) + near); // reversed Z
}

// Terrain water, the client's way (SmoothWaterFSUnified): Terrain.WaterColor
// lit by the ambient light and the whole sun (no N.L), seen through more the
// thinner the water in front of the scene behind it, then the sky reflected
// by a Fresnel-like term times WaterReflectance, plus the sun's GGX glint.
// The client draws water opaque over a copy of the scene; here it blends
// over the scene itself, so what's behind isn't bent by the ripples. Ripples
// come from the client's animated water normal maps (PlatformContent
// textures/water, x in alpha and y in green like its other normal maps).
void waterSurface(vec3 world, vec3 n, vec3 v, vec3 albedo, float sun, vec2 grid, vec3 local, inout vec3 color,
                  inout float alpha)
{
    TerrainLayer water = frame.terrainLayers[1];
    vec3 rippled = n;
    float roughness = 0.089;
    if (water.maps.y != 0u) {
        // tiling.x: tiles per stud; tiling.z: wave strength (WaterWaveSize).
        // The client's ripple maps play as a loop: this frame (maps.y)
        // turning into the next (maps.z) by blend.x.
        vec2 p = world.xz * water.tiling.x;
        vec2 q = p * 0.37 + vec2(0.31, 0.17);
        vec4 a = mix(sampleTexture(water.maps.y, p, 0u), sampleTexture(water.maps.z, p, 0u), water.blend.x);
        vec4 b = mix(sampleTexture(water.maps.y, q, 0u), sampleTexture(water.maps.z, q, 0u), water.blend.x);
        vec2 ripple = (a.wy + b.wy) - 1.0;
        rippled = normalize(n + vec3(ripple.x, 0.0, ripple.y) * water.tiling.z);
        // The map's red channel is the client's roughness, 0.089 to 1.
        roughness = 0.089 + 0.911 * (a.x + b.x) * 0.5;
    }
    // Reflections see a calmer surface: a quarter of the ripple.
    vec3 calm = normalize(mix(n, rippled, 0.25));
    float ndv = abs(dot(calm, v));
    vec3 r = reflect(-v, calm);

    vec3 lit = albedo * (cappedAmbient(frame.ambient.rgb + frame.skyAmbient.rgb * grid.x, local)
                         + ambientCube(rippled) * grid.x + frame.sunColor.rgb * sun * grid.y);
    vec3 gradient = mix(frame.skyGradientBottom.rgb, frame.skyGradientTop.rgb, clamp(r.y * 1.5882353, 0.0, 1.0));
    // Divided by the exposure that follows: the client exposes its sky and
    // the sky's reflection alike, but our sky pass doesn't expose, and the
    // reflection shouldn't outshine the sky it shows.
    vec3 reflected = mix(indoorReflection(), reflectedSky(r) * gradient, grid.x) / max(frame.ambient.w, 1e-3);
    float fresnel = clamp((clamp(0.78 - 2.5 * ndv, 0.0, 1.0) + 0.3) * water.color.w, 0.0, 1.0);
#ifdef RAYTRACING
    // Ray traced, the shore and what stands in the water: lit like the
    // scene, so exposed like it.
    vec3 scene;
    if (frame.quality.z > 0.5 && fresnel > 0.0 && tracedReflection(world, n, r, scene))
        reflected = scene;
#endif

    // How much of the scene behind the water is hidden: by the studs of
    // water in front of it (view depth, so more at grazing angles), and all
    // of it past 250 studs. GUESS: the client's opacity per stud and offset
    // (ShadingParams.xy) from WaterTransparency; alpha is 1 - transparency.
    float waterDepth = 1.0 / gl_FragCoord.w;
    float behind = viewDepth(texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r);
    float opaque = clamp(clamp((behind - waterDepth) * 0.2 * alpha + 0.2 * alpha, 0.0, 1.0)
                         + clamp(waterDepth * 0.004 - 1.0, 0.0, 1.0), 0.0, 1.0);

    vec3 glint = sunHighlight(rippled, v, frame.sunDirection.xyz, roughness, vec3(0.02)) * frame.sunColor.rgb
                 * clamp(dot(rippled, frame.sunDirection.xyz) * sun * grid.y, 0.0, 1.0);

    // scene * (1 - cover) + over * cover, done by the blend unit.
    float cover = 1.0 - (1.0 - opaque) * (1.0 - fresnel);
    vec3 over = (lit * opaque * (1.0 - fresnel) + reflected * fresnel + glint) / max(cover, 1e-3);
    color = tonemap(over * frame.ambient.w, frame.tone.x > 0.5);
    alpha = cover;
}

#ifdef DENOISE
// The picture main makes of light before the tone curve, as it goes on to
// (water aside, which the denoiser leaves alone).
vec3 finishedColor(vec3 light, vec3 v, float haze, float fog)
{
    vec3 c = tonemap(light * frame.ambient.w, frame.tone.x > 0.5);
    if (haze > 0.0)
        c = mix(c, distanceColor(-v), haze);
    return colorCorrect(mix(c, frame.fogColor.rgb, fog));
}

#endif

void main()
{
    Shade shade = shades[inShade];
    vec4 inSurface = shade.surface;
    vec4 inPattern = shade.pattern;
    uvec4 inMaps = shade.maps;
    uvec4 inExtra = shade.extra;
    vec3 inWorld = inWorldUv.xyz;
    vec3 inNormal = inNormalUv.xyz;
    vec2 inUv = vec2(inWorldUv.w, inNormalUv.w) * shade.uvTransform.xy + shade.uvTransform.zw;
    vec3 inTint = shade.tint.rgb * inVertexColor;
    uint flags = inExtra.z;
    uint alphaMode = flags & 3u;
    uint samplerIndex = (flags & flagClamp) != 0u ? 1u : 0u;
    bool decal = (flags & flagDecal) != 0u;
    bool terrain = terrainItems && (flags & flagTerrain) != 0u;

    vec3 n = normalize(inNormal);
    vec2 grid = lightGridAt(inWorld, n);
#ifdef RAYTRACING
    // No sky in a closed room (skyRayOut). The sun's rays see to the
    // grid's sun themselves.
    // Where the grid has no sky for the whole quad, no rays.
    if (frame.quality.z > 0.5 && quadAny(grid.x > 0.0) && !quadAny(skyRayOut(inWorld, normalize(inNormal))))
        grid.x = 0.0;
    if (frame.occlusion.x > 0.5)
        occlusion = tracedOcclusion(inWorld, n);
#endif
    vec3 toCamera = frame.cameraPosition.xyz - inWorld;
    float distance = length(toCamera);
    vec3 v = toCamera / max(distance, 1e-4);
    vec3 l = frame.sunDirection.xyz;
    vec3 localNormal = normalize(inLocalNormal);

    vec3 albedo = inColor.rgb;
    float alpha = inColor.a;
    if (!terrain && inMaps.x != 0u) {
        vec4 t = sampleTexture(inMaps.x, inUv, samplerIndex);
        vec3 tinted = t.rgb * inTint;
        if (alphaMode == 0u) { // overlay
            albedo = mix(inColor.rgb, tinted, t.a);
        } else if (alphaMode == 1u) { // transparency
            albedo = tinted;
            alpha *= t.a;
        } else if (alphaMode == 2u) { // tint mask
            albedo = t.rgb * inVertexColor * mix(vec3(1.0), shade.tint.rgb, t.a);
        } else {
            albedo = tinted;
        }
    }
    if (alphaTest) {
        if (alpha < 0.5)
            discard;
        alpha = 1.0;
        if (terrain) {
            // Cross-fading levels of detail: a 4x4 ordered dither against
            // the item's range (uvTransform.xy, unused by terrain otherwise).
            const float bayer[16] = float[](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0,
                                            13.0, 5.0);
            uvec2 cell = uvec2(gl_FragCoord.xy) & 3u;
            float d = (bayer[cell.y * 4u + cell.x] + 0.5) / 16.0;
            if (d < shade.uvTransform.x || d >= shade.uvTransform.y)
                discard;
        }
    }

    float terrainGlow = 0.0;
    if (terrain) {
        // Terrain meshes are in world space.
        terrainSurface(inMaps, inWorld, n, albedo, n, terrainGlow);
    } else if (!decal) {
        if (inExtra.x != 0u) {
            // Real material textures replace the procedural pattern.
            vec3 p = inStuds / max(inPattern.w, 1e-3);
            vec3 w = triplanarWeights(localNormal);
            vec4 t = triplanarColor(inExtra.x, p, w);
            // Tint masks keep the texture's own colour where alpha is clear
            // (Roblox's terrain shader does the same: mix(1, colour, a) * rgb).
            albedo = (flags & flagTintMask) != 0u ? t.rgb * mix(vec3(1.0), albedo, t.a) : albedo * t.rgb;
            if (inExtra.y != 0u)
                n = normalize(rotate(shade.rotation, triplanarNormal(inExtra.y, p, localNormal, w)));
        } else {
            int kind = int(inPattern.x + 0.5);
            if (kind != 0) {
                // Project onto the object-space plane the surface mostly faces.
                vec3 a = abs(inLocalNormal);
                vec2 p = a.x > a.y && a.x > a.z ? inStuds.zy : (a.y > a.z ? inStuds.xz : inStuds.xy);
                albedo *= detail(kind, p * inPattern.y, inPattern.z);
            }
        }

        uint face = blockFace(localNormal);
        uint surface = face < 6u ? (inExtra.w >> (2u * face)) & 3u : 0u;
        if (surface != 0u) {
            vec3 c = inStuds;
            // Across the face, in studs from its corner.
            vec2 p = face == 0u || face == 3u ? c.zy : (face == 1u || face == 4u ? c.xz : c.xy);
            uint tile = surface == 1u ? frame.surfaceTextures.x : frame.surfaceTextures.y;
            if (tile != 0u)
                albedo *= sampleTexture(tile, p, 0u).rgb * 2.0;
            else
                albedo *= studShade(p, surface);
        }
    }

    // The decals on this surface, over its colour as they would blend over
    // its picture. Where they cover it, it's lit as a decal is on its own:
    // plastic, flat, neither glowing nor reflecting.
    float decalCover = 0.0;
    if (decalLayer && frame.decals.x > 0.5) {
        vec4 d = texelFetch(decalLayerImage, ivec2(gl_FragCoord.xy), 0);
        albedo = albedo * (1.0 - d.a) + d.rgb;
        decalCover = d.a;
    }

    // Potato: the light of the face's own normal, the sun's in three hard
    // steps, and nothing else (no shadows, highlights, reflections or
    // per-pixel lights; the light map's lamps stay, or a night is black).
    if (frame.tone.y > 0.5) {
        vec3 face = normalize(inNormal);
        float steps = floor(clamp(dot(face, l), 0.0, 1.0) * 3.0 + 0.5) / 3.0;
        vec3 light = cappedAmbient(frame.ambient.rgb + frame.skyAmbient.rgb * grid.x, lightMapAt(inWorld + face * 6.0))
                     + ambientCube(face) * grid.x + frame.sunColor.rgb * steps * grid.y;
        vec3 c = tonemap(albedo * light * frame.ambient.w, frame.tone.x > 0.5);
        float fog = clamp((distance - frame.fogRange.x) / max(frame.fogRange.y - frame.fogRange.x, 1e-3), 0.0, 1.0);
        outColor = vec4(colorCorrect(mix(c, frame.fogColor.rgb, fog)), alpha);
#ifdef DENOISE
        outRest = vec4(0.0, 0.0, 0.0, alpha);
        outRestSeen = vec4(0.0, 0.0, 0.0, alpha);
        outGuide = vec4(face, 1.0 / gl_FragCoord.w);
#endif
        return;
    }

    if (!terrain && inMaps.y != 0u) {
        // Tangent-space normal map; the bitangent points up the image.
        vec3 t = inTangent.xyz - n * dot(n, inTangent.xyz);
        if (dot(t, t) > 1e-10) {
            t = normalize(t);
            vec3 b = cross(n, t) * (inTangent.w < 0.0 ? -1.0 : 1.0);
            vec3 m = sampleTexture(inMaps.y, inUv, samplerIndex).xyz * 2.0 - 1.0;
            n = normalize(t * m.x + b * m.y + n * max(m.z, 1e-3));
        }
    }
    if (decalCover > 0.0)
        n = normalize(mix(n, normalize(inNormal), decalCover));

    float specular = mix(inSurface.x, frame.decals.y, decalCover);
    float shininess = mix(inSurface.y, frame.decals.z, decalCover);
    float emissive = inSurface.z * (1.0 - decalCover);
    float reflectance = inSurface.w * (1.0 - decalCover);

    float ndl = dot(n, l);
    vec3 geometric = normalize(inNormal);
    // The sun's light is everywhere below its colour times grid.y (the light
    // grid's sun): where either is 0 it lights nothing whatever its shadow
    // says, and the shadow (a ray, or the map's taps) isn't looked at.
    bool sunShows = grid.y > 0.0 && max(frame.sunColor.r, max(frame.sunColor.g, frame.sunColor.b)) > 0.0;
    float sun = ndl > 0.0 && sunShows ? sunVisibility(inWorld, geometric) : 0.0;
    // How directly the surface faces the camera, for Fresnel.
    float ndv = max(dot(n, v), 1e-4);
    bool physical = (flags & flagPhysical) != 0u;
    float roughness, metalness = 0.0;
    if (physical) {
        roughness = clamp(inMaps.z != 0u ? sampleTexture(inMaps.z, inUv, samplerIndex).r : 0.7, 0.04, 1.0);
        metalness = clamp(inMaps.w != 0u ? sampleTexture(inMaps.w, inUv, samplerIndex).r : 0.0, 0.0, 1.0);
        if (decalCover > 0.0) {
            // A decal's plastic, as the other branch would make it.
            float plastic = frame.decals.y > 0.0 ? clamp(sqrt(2.0 / (frame.decals.z + 2.0)), 0.089, 1.0) : 1.0;
            roughness = mix(roughness, plastic, decalCover);
            metalness *= 1.0 - decalCover;
        }
    } else {
        // Matte materials get the client's roughest plastic; shinier ones
        // a roughness from their Blinn-Phong exponent (GUESS: the client's
        // per-material roughness isn't in the shaders).
        roughness = specular > 0.0 ? clamp(sqrt(2.0 / (shininess + 2.0)), 0.089, 1.0) : 1.0;
    }

    // Local lights: the light map's (6 studs out, like the grid), and near
    // the camera per pixel, fading into the light map further out. Ray
    // traced, the per-pixel ones add their diffuse light straight to the
    // picture (localLight, see localLightsPerPixel), times what the shading
    // below multiplies the light from all around by: the albedo, or a
    // physical surface's share of it the Fresnel term leaves.
    vec3 localDiffuse = lightMapAt(inWorld + geometric * 6.0);
    vec3 localLight = vec3(0.0);
    float localFade = localLightFade(distance);
    if (localFade < 1.0 && emissive < 1.0) {
        vec3 f0 = physical ? mix(vec3(0.04), albedo, metalness) : vec3(0.04 + 0.5 * specular);
        vec3 diffuseAlbedo = vec3(0.0);
#ifdef RAYTRACING
        diffuseAlbedo = physical ? physicalDiffuse(albedo, metalness, n, v, l) : albedo;
#endif
        vec3 pixelDiffuse;
        localLight = localLightsPerPixel(inWorld, geometric, n, v, roughness, f0, diffuseAlbedo, pixelDiffuse)
                     * (1.0 - localFade);
        localDiffuse = mix(pixelDiffuse, localDiffuse, localFade);
    }

    vec3 color;
    vec3 ambientLight;
#ifdef DENOISE
    // How much of the rest of the lights' light reaches the picture before
    // the tone curve: what the light map's fade and the mixes below leave.
    float restKeep = 1.0 - localFade;
#endif
    if (physical) {
        ambientLight = diffuseLight(n, ndl, 0.0, 0.0, grid, localDiffuse);
        color = physicalLight(inWorld, geometric, albedo, roughness, metalness, n, v, l,
                              frame.sunColor.rgb * sun * grid.y, ambientLight, grid.x)
                + localLight;
#ifdef RAYTRACING
        float polish = everythingReflects(ndv, decal) * (1.0 - decalCover);
        if (polish > 0.0) {
            vec3 r = reflect(-v, n);
            vec3 scene;
            if (!tracedReflection(inWorld, normalize(inNormal), r, scene)) {
                vec3 gradient
                    = mix(frame.skyGradientBottom.rgb, frame.skyGradientTop.rgb, clamp(r.y * 1.5882353, 0.0, 1.0));
                scene = mix(indoorReflection(), reflectedSky(r) * gradient, grid.x);
            }
            color = mix(color, scene, polish);
#ifdef DENOISE
            restKeep *= 1.0 - polish;
#endif
        }
#endif
    } else {
        float environmentF = 0.04 + 0.96 * pow(1.0 - ndv, 5.0) * (1.0 - roughness);
        ambientLight = diffuseLight(n, ndl, 0.0, environmentF, grid, localDiffuse);
        color = albedo * diffuseLight(n, ndl, sun, environmentF, grid, localDiffuse);
        color += sunHighlight(n, v, l, roughness, vec3(0.04 + 0.5 * specular)) * frame.sunColor.rgb
                 * clamp(ndl * sun * grid.y, 0.0, 1.0);
        color += localLight;
        // Reflectance mirrors the sky; more so at grazing angles.
        float fresnel = pow(1.0 - ndv, 5.0);
        float mirror = clamp(reflectance * (0.6 + 0.4 * fresnel), 0.0, 1.0);
#ifdef RAYTRACING
        mirror = max(mirror, everythingReflects(ndv, decal) * (1.0 - decalCover));
#endif
        // The skybox along r, looked up once and only where it shows: the
        // fully rough (most parts) see the blurred sky, and only Reflectance
        // mirrors it. Both hold for a whole part, so its pixels branch alike.
        vec3 r = reflect(-v, n);
        bool sharpShows = (roughness < 1.0 && frame.skyGradientBottom.w > 0.0) || mirror > 0.0;
        vec3 sharp = sharpShows ? reflectedSky(r) : vec3(0.0);
        vec3 environment = environmentReflection(sharp, r, roughness, grid.x);
        vec3 mirrored = vec3(0.0);
        if (mirror > 0.0) {
            vec3 gradient
                = mix(frame.skyGradientBottom.rgb, frame.skyGradientTop.rgb, clamp(r.y * 1.5882353, 0.0, 1.0));
            mirrored = mix(indoorReflection(), sharp * gradient, grid.x);
        }
#ifdef RAYTRACING
        // Ray traced, the scene takes the sky's place where the ray meets
        // it, on mirrors and where what's shiny enough shows it: from
        // SmoothPlastic's highlight up, not the faint sheen of stone.
        bool glossy = specular >= 0.15 && roughness < tracedReflectionRoughness && frame.skyGradientBottom.w > 0.0
                      && environmentF * (1.0 - roughness) * frame.skyGradientBottom.w >= tracedReflectionShare;
        vec3 scene;
        if (frame.quality.z > 0.5 && !alphaTest && (glossy || mirror > 0.0)
            && tracedReflection(inWorld, normalize(inNormal), r, scene)) {
            environment = mix(scene * frame.skyGradientBottom.w, environmentReflection(vec3(0.0), r, 1.0, grid.x),
                              roughness);
            mirrored = scene;
        }
#endif
        color += environment * environmentF;
        if (mirror > 0.0) {
            color = mix(color, mirrored, mirror);
#ifdef DENOISE
            restKeep *= 1.0 - mirror;
#endif
        }
    }

    // Neon ignores the light and glows a little past its colour.
    color = mix(color, albedo * 1.5, emissive);
#ifdef DENOISE
    restKeep *= 1.0 - emissive;
#endif
    color += albedo * terrainGlow;
#ifdef DENOISE
    vec3 light = color;
#endif
    color = tonemap(color * frame.ambient.w, frame.tone.x > 0.5);

    if (terrainItems && (flags & flagWater) != 0u)
        waterSurface(inWorld, geometric, v, albedo, sun, grid, lightMapAt(inWorld + geometric * 6.0), color, alpha);

    // Atmosphere: distant things fade into the sky behind them.
    float haze = 1.0 - exp(-distance * frame.atmosphereColor.w);
    if (haze > 0.0)
        color = mix(color, distanceColor(-v), haze);
    float fog = clamp((distance - frame.fogRange.x) / max(frame.fogRange.y - frame.fogRange.x, 1e-3), 0.0, 1.0);
    color = mix(color, frame.fogColor.rgb, fog);

    outColor = vec4(colorCorrect(color), alpha);
#ifdef DENOISE
    // The picture with the rest's light reaching and not, from what was
    // shaded (which had it as its ray found): the denoiser swaps what the
    // ray found for what the pixels around found.
    vec3 restDelta = vec3(0.0);
    float seen = max(restLightSeen, 0.0);
    if (restLightSeen >= 0.0 && !(terrainItems && (flags & flagWater) != 0u)) {
        vec3 rest = restLight * restKeep;
        restDelta = finishedColor(light + (1.0 - seen) * rest, v, haze, fog)
                    - finishedColor(light - seen * rest, v, haze, fog);
    }
    outRest = vec4(restDelta, alpha);
    outRestSeen = vec4(restDelta * seen, alpha);
    outGuide = vec4(geometric, 1.0 / gl_FragCoord.w);
#endif
}
