#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "frame.glsl"
#include "sky.glsl"

// The sky behind everything: Lighting's Sky (six faces), stars at night, the
// sun and moon sprites, and Atmosphere haze over it.
layout(location = 0) in vec3 inRay;
layout(location = 0) out vec4 outColor;

float hash(vec3 p)
{
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

// Stars: one candidate per cell of a grid over each cube face, about
// starCount of them over the whole sky, each a soft point a little over a
// pixel wide. Procedural: the client's StarsVS draws a vertex buffer of
// points we don't have.
float stars(vec3 d, float count)
{
    if (count < 1.0)
        return 0.0;
    vec3 a = abs(d);
    float major = max(a.x, max(a.y, a.z));
    vec3 c = d / major;
    vec2 p;
    float face;
    if (a.x >= major) {
        p = c.zy;
        face = d.x > 0.0 ? 0.0 : 1.0;
    } else if (a.y >= major) {
        p = c.xz;
        face = d.y > 0.0 ? 2.0 : 3.0;
    } else {
        p = c.xy;
        face = d.z > 0.0 ? 4.0 : 5.0;
    }
    float cells = max(sqrt(count / 6.0), 1.0);
    vec2 g = (p * 0.5 + 0.5) * cells;
    vec2 cell = floor(g);
    vec3 seed = vec3(cell, face);
    vec2 at = vec2(hash(seed), hash(seed + 7.31)) * 0.8 + 0.1;
    float brightness = hash(seed + 3.17);
    // Points about a pixel across, sized from how fast the direction turns
    // (smooth across faces, unlike the grid).
    float pixel = length(fwidth(d)) * 0.5 / major * cells;
    float r = length(g - cell - at) / max(pixel * 0.8, 1e-4);
    return smoothstep(1.0, 0.0, r) * brightness * brightness;
}

// A sprite of a celestial body: its texture on a square facing the camera,
// size given as tan of half its angular width. Black is clear.
vec3 sprite(vec3 d, vec4 body, uint tex)
{
    if (tex == 0u || body.w <= 0.0)
        return vec3(0.0);
    vec3 axis = normalize(body.xyz);
    float facing = dot(d, axis);
    if (facing <= 0.0)
        return vec3(0.0);
    vec3 right = normalize(cross(abs(axis.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), axis));
    vec3 up = cross(axis, right);
    vec2 p = vec2(dot(d, right), dot(d, up)) / facing / body.w;
    if (any(greaterThan(abs(p), vec2(1.0))))
        return vec3(0.0);
    return sampleTexture(tex, p * vec2(0.5, -0.5) + 0.5, 1u).rgb;
}

// An integer hash of a lattice point, steady at any distance.
float latticeHash(ivec2 p)
{
    uint h = uint(p.x) * 1597334677u ^ uint(p.y) * 3812015801u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h) * (1.0 / 4294967295.0);
}

float noise2(vec2 p)
{
    ivec2 i = ivec2(floor(p));
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = latticeHash(i), b = latticeHash(i + ivec2(1, 0));
    float c = latticeHash(i + ivec2(0, 1)), e = latticeHash(i + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, e, f.x), f.y);
}

// Terrain's Clouds as one layer high over the map, drifting with time:
// fractal noise, as much sky covered as Cover asks, as thick as Density,
// lit by the sun on top and the ambient light below. GUESS: the client
// marches volumetric clouds through distance-field textures we don't
// have; this keeps their amount, colour and light.
vec4 cloudLayer(vec3 d)
{
    if (frame.clouds.z < 0.5 || d.y <= 0.02)
        return vec4(0.0);
    // Bent down towards the horizon, as over a round world, so the far
    // clouds don't smear into streaks.
    vec2 p = d.xz / (d.y + 0.2) * 0.8 + vec2(frame.depthParams.z * 0.004, 0.0);
    float n = 0.0, amplitude = 0.5;
    for (int i = 0; i < 5; ++i) {
        n += noise2(p) * amplitude;
        p = p * 2.03 + vec2(1.7, 9.2);
        amplitude *= 0.5;
    }
    float cover = frame.clouds.x;
    float shape = smoothstep(1.0 - cover, 1.0 - cover + 0.25, n);
    float alpha = shape * (0.4 + 0.6 * frame.clouds.y) * smoothstep(0.02, 0.25, d.y);
    // Thicker clouds are darker underneath.
    float thickness = shape * frame.clouds.y;
    vec3 light = frame.sunColor.rgb * (1.0 - 0.6 * thickness) + frame.ambient.rgb + frame.skyAmbient.rgb;
    return vec4(frame.cloudColor.rgb * light, alpha);
}

void main()
{
    vec3 d = normalize(inRay);
    vec3 color = skyboxColor(d);
    if (frame.skyTint.w > 0.0)
        color += vec3(stars(d, frame.skyParams.x) * frame.skyTint.w);
    if (frame.skyParams.y > 0.5) {
        color += sprite(d, frame.sunSprite, frame.skyFaces1.z) * frame.skyParams.z;
        color += sprite(d, frame.moonSprite, frame.skyFaces1.w) * frame.skyParams.w;
    }
    vec4 cloud = cloudLayer(d);
    color = mix(color, cloud.rgb, cloud.a);
    color = applyAtmosphereToSky(color, d);
    outColor = vec4(colorCorrect(color), 1.0);
}
