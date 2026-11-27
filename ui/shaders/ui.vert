#version 460
#extension GL_GOOGLE_include_directive : require

#include "ui.glsl"

layout(location = 0) out vec2 outLocal;  // pixels from the quad's top-left
layout(location = 1) flat out uint outQuad;
layout(location = 2) out vec2 outCanvas; // where on its screen or surface (after rotation), for clipping

void main()
{
    Quad q = quads[gl_InstanceIndex];
    // Triangle strip corners (0,0) (1,0) (0,1) (1,1), grown by a pixel so
    // edges at fractional positions get antialiased coverage.
    vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    vec2 local = corner * (q.rect.zw + 2.0) - 1.0;
    vec2 pos = q.rect.xy + local;
    vec2 d = pos - q.rotation.zw;
    pos = q.rotation.zw + vec2(d.x * q.rotation.x - d.y * q.rotation.y, d.x * q.rotation.y + d.y * q.rotation.x);
    // On the screen the GUI is nearest of all (reversed Z), so the depth
    // test always passes it.
    vec4 position = vec4(pos / push.params.xy * 2.0 - 1.0, 1.0, 1.0);
    uint layer = q.info.y >> 16;
    if (layer != 0u && push.params.w != 0.0) {
        Layer l = layers[push.layerBase.x + layer - 1u];
        bool hidden = l.axisX.w != 0.0;
        if (l.origin.w == 0.0) {
            // A billboard faces the camera: all of it is as deep as its point.
            vec4 clip = push.viewProjection * vec4(l.origin.xyz, 1.0);
            if (hidden && clip.w > 0.0)
                position.z = clamp(clip.z / clip.w, 0.0, 1.0);
        } else {
            position = push.viewProjection * vec4(l.origin.xyz + pos.x * l.axisX.xyz + pos.y * l.axisY.xyz, 1.0);
            if (!hidden)
                position.z = position.w;
        }
    }
    gl_Position = position;
    outLocal = local;
    outQuad = gl_InstanceIndex;
    outCanvas = pos;
}
