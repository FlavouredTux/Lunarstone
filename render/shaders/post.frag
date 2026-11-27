#version 450
#extension GL_GOOGLE_include_directive : require

// Lighting's post effects (BloomEffect, BlurEffect, SunRaysEffect,
// DepthOfFieldEffect) and ColorCorrection, the way the client's image
// processing shaders do them (ImageProcess*FS, DownSampleGlow*, UpSampleTent,
// SunRays*FS, RadialBlur*FS in shaders_glsl3.pack). The scene arrives tone
// mapped, so light above the curve's top is gone; its HDR is taken back
// through the inverse of the curve, which recovers up to 4 (white), as
// much as the client's own scene target holds.
// Every pass is one mode of this shader (PostPass.cpp says which reads what).

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D source; // the scene (sRGB, read as linear) or a smaller level
layout(set = 0, binding = 1) uniform sampler2D bloomTexture;
layout(set = 0, binding = 2) uniform sampler2D blurTexture;
layout(set = 0, binding = 3) uniform sampler2D raysTexture;
layout(set = 0, binding = 4) uniform sampler2D depthTexture; // reversed Z, 0 at the far plane

layout(push_constant) uniform Post {
    vec4 texel;  // xy: the source's texel size in uv
    vec4 params; // by mode, see below
    vec4 params2;
    vec4 extra[3]; // composite: rows of the ColorCorrection matrix; rays: see modeRays
    vec4 params3;
    int mode;
} post;

const int modeBright = 0;    // params: bloom Intensity, Threshold; params3.w: 1 for the Retro tone curve
const int modeDown = 1;      // a 2x smaller copy, box filtered
const int modeUp = 2;        // params: tent radius in source texels, weight
const int modeRays = 3;      // params: the sun's uv, g, g squared; params2: sun colour, intensity; params3: near, far;
                             // extra: the view's forward, right and up (to the screen's edges), w: towards the sun
const int modeRadial = 4;    // params: the sun's uv, taps, length
const int modeComposite = 5; // params: bloom weight, blur amount, rays on, 1 when re-tone-mapping;
                             // params2: DoF near edge, far edge, NearIntensity, FarIntensity; params3: near, far, DoF on,
                             // 1 for the Retro tone curve

// mesh.frag's tone curve, and its inverse.
#include "tone.glsl"

// The client's bloom bright pass (DownSampleGlowTemporal8x8FS): light
// above Threshold, by its brightest channel, times Intensity.
vec3 bright(vec2 uv)
{
    vec3 hdr = untonemap(texture(source, uv).rgb, post.params3.w > 0.5);
    float peak = max(max(hdr.r, hdr.g), hdr.b);
    return hdr * (max(peak - post.params.y, 0.0) / (peak + 0.001) * post.params.x);
}

vec3 box(vec2 uv)
{
    vec2 d = post.texel.xy;
    return 0.25 * (texture(source, uv + vec2(-d.x, -d.y)).rgb + texture(source, uv + vec2(d.x, -d.y)).rgb
                   + texture(source, uv + vec2(-d.x, d.y)).rgb + texture(source, uv + vec2(d.x, d.y)).rgb);
}

// UpSampleTentFS: a 3x3 tent.
vec3 tent(vec2 uv, float radius)
{
    vec2 d = post.texel.xy * radius;
    vec3 sum = texture(source, uv).rgb * 4.0;
    sum += (texture(source, uv + vec2(-d.x, 0.0)).rgb + texture(source, uv + vec2(d.x, 0.0)).rgb
            + texture(source, uv + vec2(0.0, -d.y)).rgb + texture(source, uv + vec2(0.0, d.y)).rgb)
           * 2.0;
    sum += texture(source, uv - d).rgb + texture(source, uv + d).rgb + texture(source, uv + vec2(d.x, -d.y)).rgb
           + texture(source, uv + vec2(-d.x, d.y)).rgb;
    return sum * 0.0625;
}

float linearDepth(float z)
{
    float near = post.params3.x;
    float far = post.params3.y;
    return near * far / (z * (far - near) + near);
}

// The step from uv towards the sun, a sixth (or a tap's share) of the way
// there, or of the way to the screen's edge when the sun is off it.
vec2 sunStep(vec2 uv, vec2 sun, float taps, float length)
{
    vec2 d = sun - uv;
    vec2 edge = vec2(d.x > 0.0 ? (1.0 - uv.x) / d.x : (d.x < 0.0 ? -uv.x / d.x : 1.0),
                     d.y > 0.0 ? (1.0 - uv.y) / d.y : (d.y < 0.0 ? -uv.y / d.y : 1.0));
    return d * min(min(edge.x, edge.y), 1.0) * length / taps;
}

// Interleaved gradient noise, for jittering the rays' first tap.
float noise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main()
{
    if (post.mode == modeBright) {
        vec2 d = post.texel.xy;
        vec3 c = 0.25 * (bright(inUv + vec2(-d.x, -d.y)) + bright(inUv + vec2(d.x, -d.y))
                         + bright(inUv + vec2(-d.x, d.y)) + bright(inUv + vec2(d.x, d.y)));
        outColor = vec4(c, 1.0);
    } else if (post.mode == modeDown) {
        outColor = vec4(box(inUv), 1.0);
    } else if (post.mode == modeUp) {
        outColor = vec4(tent(inUv, post.params.x) * post.params.y, 1.0);
    } else if (post.mode == modeRays) {
        // SunRays6FS: of six taps from here towards the sun, the share that
        // see past everything near (the client's linear depth caps at 500
        // studs), times a Henyey-Greenstein lobe around the sun. GUESS: the
        // lobe's scale, which the client sets on the CPU (params2.w has it).
        vec2 sun = post.params.xy;
        vec2 stepUv = sunStep(inUv, sun, 6.0, 1.0);
        vec2 at = inUv + stepUv * noise(gl_FragCoord.xy);
        float open = 0.0;
        for (int i = 0; i < 6; ++i) {
            float z = texture(depthTexture, at).x;
            open += (z <= 0.0 || linearDepth(z) >= 499.5) ? 1.0 : 0.0;
            at += stepUv;
        }
        vec3 ray = normalize(post.extra[0].xyz + (inUv.x * 2.0 - 1.0) * post.extra[1].xyz
                             - (inUv.y * 2.0 - 1.0) * post.extra[2].xyz);
        vec3 toSun = vec3(post.extra[0].w, post.extra[1].w, post.extra[2].w);
        // The lobe over its peak (1 looking at the sun): ((1 - g) s)^3.
        float g = post.params.z;
        float s = inversesqrt(max(1.0 + post.params.w - 2.0 * g * dot(ray, toSun), 1e-4));
        float lobe = pow((1.0 - g) * s, 3.0);
        outColor = vec4(post.params2.rgb * (open / 6.0 * lobe * post.params2.w), 1.0);
    } else if (post.mode == modeRadial) {
        // RadialBlur*FS: an average of taps towards the sun.
        float taps = post.params.z;
        vec2 stepUv = sunStep(inUv, post.params.xy, taps, post.params.w);
        vec3 sum = vec3(0.0);
        vec2 at = inUv;
        for (int i = 0; i < int(taps); ++i) {
            at += stepUv;
            sum += texture(source, clamp(at, vec2(0.0), vec2(1.0))).rgb;
        }
        outColor = vec4(sum / taps, 1.0);
    } else {
        vec3 c = texture(source, inUv).rgb;
        float amount = post.params.y;
        if (post.params3.z > 0.5) {
            // DepthOfFieldEffect: sharp within InFocusRadius of
            // FocusDistance, blurring out past it on either side.
            float z = textureLod(depthTexture, inUv, 0.0).x;
            float d = z <= 0.0 ? 1e9 : linearDepth(z);
            float nearEdge = post.params2.x;
            float farEdge = post.params2.y;
            float nearBlur = clamp((nearEdge - d) / max(nearEdge * 0.5, 1.0), 0.0, 1.0) * post.params2.z;
            float farBlur = clamp((d - farEdge) / max(farEdge, 10.0), 0.0, 1.0) * post.params2.w;
            amount = max(amount, max(nearBlur, farBlur));
        }
        if (amount > 0.0)
            c = mix(c, texture(blurTexture, inUv).rgb, clamp(amount, 0.0, 1.0));
        if (post.params.w > 0.5) {
            // ImageProcessBloomFS / FXCompositing: bloom and rays join the
            // light before the tone curve.
            vec3 hdr = untonemap(c, post.params3.w > 0.5);
            if (post.params.x > 0.0)
                hdr += texture(bloomTexture, inUv).rgb * post.params.x;
            if (post.params.z > 0.5)
                hdr += texture(raysTexture, inUv).rgb;
            c = tonemap(hdr, post.params3.w > 0.5);
        }
        // ColorCorrection, in sRGB.
        vec3 srgb = mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
        vec4 x = vec4(srgb, 1.0);
        srgb = clamp(vec3(dot(post.extra[0], x), dot(post.extra[1], x), dot(post.extra[2], x)), 0.0, 1.0);
        outColor = vec4(mix(srgb / 12.92, pow((srgb + 0.055) / 1.055, vec3(2.4)), step(0.04045, srgb)), 1.0);
    }
}
