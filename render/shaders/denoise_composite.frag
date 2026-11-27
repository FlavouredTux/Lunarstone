#version 450

// The denoiser's last step (denoise.comp), drawn over the scene twice: each
// pixel's picture had the rest of its lights' light as its own shared ray
// found it, and this swaps that for what the denoised rays around it found.
// A normalized colour target clamps what's drawn to [0, 1] before blending,
// so what the swap adds goes on in one draw (added) and what it takes away
// in the other (subtracted from the scene).
layout(constant_id = 0) const bool takeAway = false;

layout(location = 0) in vec2 inUv;

layout(set = 0, binding = 0) uniform sampler2D restDelta;
layout(set = 0, binding = 1) uniform sampler2D restDeltaSeen;
layout(set = 0, binding = 3) uniform sampler2D denoised;

layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    float seen = texelFetch(denoised, p, 0).r;
    if (seen < 0.0)
        discard;
    vec3 change = texelFetch(restDelta, p, 0).rgb * seen - texelFetch(restDeltaSeen, p, 0).rgb;
    outColor = vec4(max(takeAway ? -change : change, vec3(0.0)), 0.0);
}
