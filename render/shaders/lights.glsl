// Local lights (PointLight, SpotLight, SurfaceLight; render::LocalLights),
// shared by the mesh and effect shaders. Include after frame.glsl.

// The light grid's local light (render::LightGrid), the client's
// LightMapTexture: colour in rgb, its strength over 120 in alpha. u is world
// x, v world z and w world y, wrapped like the sky and sun grid.
layout(set = 0, binding = 9) uniform sampler3D lightMap;

// The light map where a point is (the grid's voxels are 4 studs): what the
// client's shaders add to the ambient light, rgb * a * 120. Nothing outside
// the grid.
vec3 lightMapAt(vec3 p)
{
    if (frame.lightMap.x < 0.5)
        return vec3(0.0);
    if (any(lessThan(p, frame.lightGridMin.xyz)) || any(greaterThan(p, frame.lightGridMax.xyz)))
        return vec3(0.0);
    vec4 t = texture(lightMap, p.xzy * frame.lightGridScale.xyz);
    return t.rgb * (t.a * 120.0);
}

// The ambient light with the light map's added, as the client's grid
// shaders cap it (min(local + ambient + sky ambient * skylight,
// LightConfig0.w)); uncapped when the cap is 0.
vec3 cappedAmbient(vec3 ambient, vec3 local)
{
    vec3 sum = ambient + local;
    return frame.lightMap.y > 0.0 ? min(sum, vec3(frame.lightMap.y)) : sum;
}

// One light as the GPU has it (render::LocalLight).
struct LocalLight {
    vec4 positionReach; // xyz; w: studs from it past which it lights nothing (its range, plus a
                        // rectangle's half diagonal)
    vec4 colorStart;    // linear colour; w: studs from it before the falloff starts
    vec4 direction;     // where its cone points; w: cosine of the cone's half angle, 0 without one
    vec4 axisU;         // a SurfaceLight's rectangle: axis, w: half size
    vec4 axisV;
    vec4 flags;         // x: 1 when its shadows are traced; y: its first cube face in the shadow atlas, or -1;
                        // z: unused; w: the rectangle's sqrt(half u * half v), 0 for the others
    vec4 falloff;       // x: range in studs; y: 1 / (range - start); z: 1 / (1 - direction.w); w: unused
};
layout(set = 0, binding = 10, std430) readonly buffer LocalLights
{
    LocalLight localLights[];
};

// Whether a light can reach a point at all: what the strength below is
// sure to be 0 past, looked at before the rest of the light is read.
bool localLightReaches(uint index, vec3 p)
{
    vec4 positionReach = localLights[index].positionReach;
    vec3 away = p - positionReach.xyz;
    return dot(away, away) < positionReach.w * positionReach.w;
}

// render::LightClusters: a word per cluster (count in the low 12 bits,
// first index above), then the light indices.
layout(set = 0, binding = 11, std430) readonly buffer LightClusterWords
{
    uint lightClusters[];
};

// Shadowed lights' cube maps with Future lighting (not ray traced): six
// faces of the atlas each, whose world to atlas matrices are at the light's
// flags.y on (+X, -X, +Y, -Y, +Z, -Z), depth compared.
layout(set = 0, binding = 12) uniform sampler2DShadow localShadowAtlas;
layout(set = 0, binding = 13, std430) readonly buffer LocalShadowMatrices
{
    mat4 localShadowMatrices[];
};

// How much of a light with a cube map reaches a surface at world
// (geometric normal n) distance from it: the face it's seen through, four
// filtered taps a texel apart. Pushed off the surface by more the farther
// it is, as the map's texels grow.
float localShadowMapped(LocalLight light, vec3 world, vec3 n, float distance)
{
    vec3 p = world + n * (0.04 + 0.012 * distance);
    vec3 d = p - light.positionReach.xyz;
    vec3 a = abs(d);
    int face = a.x >= a.y && a.x >= a.z ? (d.x > 0.0 ? 0 : 1) : (a.y >= a.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5));
    vec4 s = localShadowMatrices[int(light.flags.y) + face] * vec4(p, 1.0);
    vec3 c = s.xyz / s.w;
    vec2 texel = 0.5 / vec2(textureSize(localShadowAtlas, 0));
    float lit = texture(localShadowAtlas, vec3(c.xy + vec2(-texel.x, -texel.y), c.z))
                + texture(localShadowAtlas, vec3(c.xy + vec2(texel.x, -texel.y), c.z))
                + texture(localShadowAtlas, vec3(c.xy + vec2(-texel.x, texel.y), c.z))
                + texture(localShadowAtlas, vec3(c.xy + vec2(texel.x, texel.y), c.z));
    return lit * 0.25;
}

// How strongly a light lights a surface at p facing n, 0 to 1 (to be
// multiplied by its colour), and the way to it (l) and how far (distance):
// the client's Future shaders' falloff (DefaultUnified*FS in the Metal
// pack), 110 / (d^2 + 110) over the distance d past where it starts, times
// a smooth cut-off towards the range, the cone squared and N.L, which a
// rectangle wraps round by its size over the distance. A rectangle lights
// from its nearest point.
float localLightStrength(LocalLight light, vec3 p, vec3 n, out vec3 l, out float distance)
{
    vec3 nearest = light.positionReach.xyz;
    // Only a SurfaceLight has a rectangle, and a light is the same for
    // every pixel that looks at it: the others skip this as one.
    if (light.axisU.w + light.axisV.w > 0.0) {
        vec3 away = p - nearest;
        nearest += light.axisU.xyz * clamp(dot(away, light.axisU.xyz), -light.axisU.w, light.axisU.w)
                   + light.axisV.xyz * clamp(dot(away, light.axisV.xyz), -light.axisV.w, light.axisV.w);
    }
    vec3 toLight = nearest - p;
    float d2 = max(dot(toLight, toLight), 1e-6);
    float inverse = inversesqrt(d2);
    distance = d2 * inverse;
    l = toLight * inverse;
    float range = light.falloff.x, start = light.colorStart.w;
    float past = clamp(distance, start, range) - start;
    float t = past * light.falloff.y;
    float window = t * t * (2.0 * t - 3.0) + 1.0;
    float spot = 1.0;
    if (light.direction.w != 0.0) {
        float c = clamp((dot(light.direction.xyz, -l) - light.direction.w) * light.falloff.z, 0.0, 1.0);
        spot = c * c;
    }
    float wrap = inverse > 1000.0 ? 0.0 : clamp(light.flags.w * inverse + dot(n, l), 0.0, 1.0);
    return min(110.0 * window * spot * wrap / (past * past + 110.0), 1.0);
}

// The cluster a pixel is in: its first light index and how many.
uvec2 lightCluster(vec2 fragCoord, float depth)
{
    uvec2 tile = min(uvec2(fragCoord / frame.lightClusters.z), uvec2(frame.lightClusters.xy) - 1u);
    float s = log2(max(depth, 1e-4) * 0.1) * 1.7095 + 1.0;
    uint slice = uint(clamp(s, 0.0, frame.lightClusters.w - 1.0));
    uint cells = uint(frame.lightClusters.x) * uint(frame.lightClusters.y) * uint(frame.lightClusters.w);
    uint word = lightClusters[(slice * uint(frame.lightClusters.y) + tile.y) * uint(frame.lightClusters.x) + tile.x];
    return uvec2(cells + (word >> 12), word & 4095u);
}

// How far towards the light grid's local light per-pixel lights have gone
// at this distance from the camera: the client's localLightDist and
// hybridLerpSlope. 1 without per-pixel lights.
float localLightFade(float distance)
{
    if (frame.localLights.y < 0.5)
        return 1.0;
    return clamp((distance - frame.localLights.z) * frame.localLights.w, 0.0, 1.0);
}
