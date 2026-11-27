// Shared by ui.vert and ui.frag: one Quad per instance, matching
// lunar::ui's GpuQuad (std430).
struct Quad {
    vec4 rect;     // x, y, width, height in pixels
    vec4 uv;       // u0, v0, u1, v1
    vec4 color;    // sRGB, straight alpha
    vec4 radii;    // top-left, top-right, bottom-right, bottom-left
    vec4 clip;     // x0, y0, x1, y1
    vec4 rotation; // cos, sin, pivot x, pivot y
    vec4 gradient; // cos, sin, offset x, offset y of a UIGradient
    uvec4 info;    // texture, kind | pixelated << 8 | layer << 16, gradient row + 1 (0 = none), stroke width (float bits)
};

// Where the quads of one layer sit in the world (lunar::ui::WorldLayer),
// stored after the quads in the same buffer, one Quad's size each.
struct Layer {
    vec4 origin; // w: kind, 0 billboard (screen pixels at this point's depth), 1 surface (canvas pixels)
    vec4 axisX;  // world per canvas pixel; w: 1 when the scene's depth hides it
    vec4 axisY;
    vec4 tint;   // times the colour
    vec4 unused[4];
};

layout(std430, set = 0, binding = 2) readonly buffer Quads {
    Quad quads[];
};
layout(std430, set = 0, binding = 2) readonly buffer Layers {
    Layer layers[];
};

layout(push_constant) uniform Push {
    vec4 params; // viewport width, height, gradient rows, 1 when viewProjection is the scene's
    uvec4 layerBase; // x: where the layers start in the buffer
    mat4 viewProjection; // reversed Z, as the scene was drawn
} push;
