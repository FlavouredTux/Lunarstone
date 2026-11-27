#version 460

// Lunarstone's map (MapRenderer): flat-coloured instanced shapes, drawn as
// faces and then as outlines.

layout(location = 0) in vec3 inPosition; // the shape's space
layout(location = 1) in vec4 inColor;    // sRGB; alpha says what it is (MapKind)
// Per instance: the model matrix's rows and a colour.
layout(location = 2) in vec4 inRow0;
layout(location = 3) in vec4 inRow1;
layout(location = 4) in vec4 inRow2;
layout(location = 5) in vec4 inTint;

layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec4 eye;    // xyz: the camera's position (w = 1) or the direction towards it (w = 0)
    vec4 light;  // xyz: towards the sun
    vec4 params; // x: the map's lowest point, y: 1 / its height, z: what's drawn (see map.frag)
    vec4 lines;  // x: pixels a stud covers (at one stud away in perspective), y: terrain cell studs
} push;

layout(location = 0) out vec3 outWorld;
layout(location = 1) out vec3 outColor;
layout(location = 2) flat out float outKind;

void main()
{
    vec4 p = vec4(inPosition, 1.0);
    outWorld = vec3(dot(inRow0, p), dot(inRow1, p), dot(inRow2, p));
    outColor = inColor.rgb * inTint.rgb;
    outKind = inColor.a;
    gl_Position = push.viewProjection * vec4(outWorld, 1.0);
    // Outlines a hair nearer than the faces they lie on (reversed Z: nearer is larger).
    if (push.params.z > 0.5) {
        if (push.eye.w > 0.5)
            gl_Position.z *= 1.006;
        else
            gl_Position.z += 0.0003 * gl_Position.w;
    }
}
