#version 460

// Lunarstone's map (MapRenderer): a dark massing model. Faces are lit by
// their own normal, worked out from the screen-space slope of the
// position, so corners can be shared: parts lighter on top than on their
// sides, terrain lighter where it's steep or high. Outlines: terrain shows
// its triangles in lighter lines, parts their folds in darker ones. The
// target holds sRGB-encoded values, the way UI images do. GUESS: every
// number here is picked by eye.

layout(location = 0) in vec3 inWorld;
layout(location = 1) in vec3 inColor;
layout(location = 2) flat in float inKind; // 0 terrain, 0.5 water, 1 part

layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec4 eye;
    vec4 light;
    vec4 params; // z: 0 faces, 1 outlines, 2 highlight faces, 3 highlight outlines
    vec4 lines;  // x: pixels a stud covers (at one stud away in perspective), y: terrain cell studs
} push;

layout(location = 0) out vec4 outColor;

void main()
{
    int mode = int(push.params.z + 0.5);
    bool terrain = inKind < 0.25;
    bool water = !terrain && inKind < 0.75;
    if (mode == 2) {
        outColor = vec4(0.3, 0.95, 0.45, 0.22);
        return;
    }
    if (mode == 3) {
        outColor = vec4(0.45, 1.0, 0.55, 1.0);
        return;
    }
    if (mode == 1) {
        if (water)
            discard;
        // Lines fade out where they'd crowd together on screen.
        float perStud = push.eye.w > 0.5 ? push.lines.x / max(distance(push.eye.xyz, inWorld), 1e-3) : push.lines.x;
        float cell = terrain ? push.lines.y : 6.0;
        float alpha = clamp((perStud * cell - 4.0) / 10.0, 0.0, 1.0);
        if (alpha <= 0.0)
            discard;
        outColor = vec4(terrain ? inColor * 1.7 + 0.04 : inColor * 0.4, alpha * (terrain ? 0.85 : 1.0));
        return;
    }

    vec3 n = normalize(cross(dFdx(inWorld), dFdy(inWorld)));
    vec3 toEye = push.eye.w > 0.5 ? push.eye.xyz - inWorld : push.eye.xyz;
    if (dot(n, toEye) < 0.0)
        n = -n;
    float up = clamp(n.y, 0.0, 1.0);
    float sun = max(dot(n, push.light.xyz), 0.0);
    float h = clamp((inWorld.y - push.params.x) * push.params.y, 0.0, 1.0);
    float shade;
    if (terrain)
        shade = (0.85 + 0.8 * (1.0 - up) + 0.1 * sun) * mix(0.9, 1.3, h);
    else if (water)
        shade = 1.0;
    else
        shade = 0.6 + 0.5 * up + 0.15 * sun;
    outColor = vec4(min(inColor * shade, vec3(1.0)), 1.0);
}
