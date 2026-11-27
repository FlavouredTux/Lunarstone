#version 450

// Every highlight over the scene at once (HighlightPass), from the mask of
// which highlight each pixel is under: a highlight's fill where the mask is
// its own, its outline where it isn't but is within `radius` pixels. Where
// several reach a pixel they go over each other in the order of their ids,
// as if each were blended on its own.
layout(set = 0, binding = 0) uniform usampler2D mask;
// The ids in each 8 x 8 tile of it, a bit each (highlight_tiles.comp).
layout(set = 0, binding = 2) uniform usampler2D tiles;
// Indexed by id (0, no highlight, is unused).
layout(set = 0, binding = 1, std140) uniform Styles
{
    vec4 fill[32];    // linear RGB, alpha 1 - FillTransparency
    vec4 outline[32]; // linear RGB, alpha 1 - OutlineTransparency
}
styles;

layout(push_constant) uniform Push
{
    int radius;
}
push;

// Premultiplied: the blend adds it over what's left of the scene (1 - alpha).
layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 at = ivec2(gl_FragCoord.xy);
    // Most of the picture has no highlight anywhere near: the ids of the
    // pixel's tile and those around it, which the outline can't reach past,
    // say so before any of the search below.
    ivec2 tile = at >> 3;
    ivec2 lastTile = textureSize(tiles, 0) - 1;
    uint candidates = 0u;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            candidates |= texelFetch(tiles, clamp(tile + ivec2(x, y), ivec2(0), lastTile), 0).r;
    if (candidates == 0u)
        discard;
    uint center = texelFetch(mask, at, 0).r;
    // The ids within the outline's reach, a bit each: every texel of the
    // disk, four at a time. A gather at the corner of four texels returns
    // them as (x, y + 1), (x + 1, y + 1), (x + 1, y), (x, y); the sampler
    // clamps to the edge.
    int r = push.radius;
    int reach = r * r + r;
    vec2 texel = 1.0 / vec2(textureSize(mask, 0));
    uint near = 0u;
    for (int y = -r; y <= r; y += 2) {
        for (int x = -r; x <= r; x += 2) {
            uvec4 ids = textureGather(mask, (vec2(at + ivec2(x, y)) + 1.0) * texel);
            // The second column and row overshoot the disk's box when it is
            // an odd number of texels across: the disk test takes them out.
            int x1 = x + 1, y1 = y + 1;
            if (x * x + y1 * y1 <= reach)
                near |= 1u << ids.x;
            if (x1 * x1 + y1 * y1 <= reach)
                near |= 1u << ids.y;
            if (x1 * x1 + y * y <= reach)
                near |= 1u << ids.z;
            if (x * x + y * y <= reach)
                near |= 1u << ids.w;
        }
    }
    near &= ~1u; // 0: no highlight
    if (near == 0u)
        discard;
    // The highlights over each other, lowest id first, as one premultiplied
    // colour and how much of the scene it leaves.
    vec3 color = vec3(0.0);
    float left = 1.0;
    while (near != 0u) {
        int id = findLSB(near);
        near &= near - 1u;
        vec4 layer = uint(id) == center ? styles.fill[id] : styles.outline[id];
        if (layer.a <= 0.0)
            continue;
        color = layer.rgb * layer.a + color * (1.0 - layer.a);
        left *= 1.0 - layer.a;
    }
    if (left >= 1.0)
        discard;
    outColor = vec4(color, 1.0 - left);
}
