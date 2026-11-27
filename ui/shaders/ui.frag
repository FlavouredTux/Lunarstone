#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "ui.glsl"

layout(constant_id = 0) const int textureCount = 4;
// Off when the target is an _SRGB view: the colour is then converted to
// linear for the hardware to encode again.
layout(constant_id = 1) const bool srgbTarget = false;

layout(set = 0, binding = 0) uniform texture2D textures[textureCount];
layout(set = 0, binding = 1) uniform sampler samplers[2]; // linear, nearest

layout(location = 0) in vec2 inLocal;
layout(location = 1) flat in uint inQuad;
layout(location = 2) in vec2 inCanvas;
layout(location = 0) out vec4 outColor;

// Signed distance to a rounded box centred on the origin; radii per corner.
float roundedBox(vec2 p, vec2 halfSize, vec4 radii)
{
    // radii: tl, tr, br, bl; y points down.
    float r = p.x < 0.0 ? (p.y < 0.0 ? radii.x : radii.w) : (p.y < 0.0 ? radii.y : radii.z);
    r = min(r, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

vec3 toLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

vec3 toSrgb(vec3 c)
{
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThan(c, vec3(0.0031308)));
}

void main()
{
    Quad q = quads[inQuad];
    vec2 size = q.rect.zw;
    vec2 p = inLocal; // this pixel centre, relative to the top-left

    uint kind = q.info.y & 0xFFu;
    float coverage;
    if (kind == 3u) {
        // A shadow: the box inside the rect, its edge blurred across uv.x.
        float blur = max(q.uv.x, 1.0);
        float d = roundedBox(p - size * 0.5, max(size * 0.5 - blur * 0.5, vec2(0.0)), q.radii);
        coverage = 1.0 - smoothstep(-blur * 0.5, blur * 0.5, d);
    } else {
        float d = roundedBox(p - size * 0.5, size * 0.5, q.radii);
        coverage = clamp(0.5 - d, 0.0, 1.0);
        float stroke = uintBitsToFloat(q.info.w);
        if (stroke > 0.0)
            coverage *= clamp(0.5 + d + stroke, 0.0, 1.0);
    }

    // Clipping in its screen's or surface's space (after rotation), with soft edges.
    vec2 frag = inCanvas;
    vec4 c = q.clip;
    coverage *= clamp(frag.x - c.x + 0.5, 0.0, 1.0) * clamp(c.z - frag.x + 0.5, 0.0, 1.0);
    coverage *= clamp(frag.y - c.y + 0.5, 0.0, 1.0) * clamp(c.w - frag.y + 0.5, 0.0, 1.0);
    if (coverage <= 0.0)
        discard;

    vec4 color = q.color;
    uint samplerIndex = (q.info.y >> 8) & 1u;
    if (kind == 1u || kind == 2u) {
        vec2 t = clamp(p / max(size, vec2(1.0)), 0.0, 1.0);
        vec2 uv = mix(q.uv.xy, q.uv.zw, t);
        vec4 texel = texture(sampler2D(textures[nonuniformEXT(q.info.x)], samplers[samplerIndex]), uv);
        // A premultiplied picture's see-through pixels, back to the straight
        // alpha everything here blends with.
        if (((q.info.y >> 9) & 1u) != 0u && texel.a > 0.0 && texel.a < 1.0)
            texel.rgb = toSrgb(min(toLinear(texel.rgb) / texel.a, vec3(1.0)));
        if (kind == 2u)
            color.a *= texel.r;
        else
            color *= texel;
    }
    if (q.info.z != 0u) {
        // UIGradient: position along the rotated x axis, 0 to 1 across the box.
        vec2 n = p / max(size, vec2(1.0)) - 0.5 - q.gradient.zw;
        float g = n.x * q.gradient.x + n.y * q.gradient.y + 0.5;
        float row = (float(q.info.z) - 0.5) / push.params.z;
        vec4 ramp = texture(sampler2D(textures[1], samplers[0]), vec2(clamp(g, 0.0, 1.0) * 255.0 / 256.0 + 0.5 / 256.0, row));
        color *= ramp;
    }
    uint layer = q.info.y >> 16;
    if (layer != 0u && push.params.w != 0.0)
        color.rgb = min(color.rgb * layers[push.layerBase.x + layer - 1u].tint.rgb, vec3(1.0));
    color.a *= coverage;
    outColor = vec4(srgbTarget ? color.rgb : toLinear(color.rgb), color.a);
}
