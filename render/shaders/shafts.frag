#version 460
#extension GL_EXT_ray_query : require

// Sun rays with ray queries (PostPass, under --raytracing): light scattered
// towards the camera by the air along each view ray, out to the first
// surface or maxDistance, wherever the sun reaches it. Shafts come from
// whatever casts shadows, on screen or not, looking any way. Not the
// client's (it only has the screen-space SunRays*FS); its SunRaysEffect
// sets how strong (params2.w) and how wide around the sun (g).

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D depthTexture; // reversed Z, 0 at the far plane
layout(set = 0, binding = 1) uniform accelerationStructureEXT rayScene;

// post.frag's push constants, read this way here.
layout(push_constant) uniform Post {
    vec4 texel;
    vec4 params;   // the camera's x, y; g, g squared
    vec4 params2;  // sun colour; scattering per stud
    vec4 extra[3]; // the view's forward, right and up (to the screen's edges); w: towards the sun
    vec4 params3;  // near, far plane; studs marched at most; the camera's z
    int mode;
} post;

const uint maskCasts = 1u; // mesh.frag's: what casts sun shadows
const int steps = 24;

float linearDepth(float z)
{
    float near = post.params3.x;
    float far = post.params3.y;
    return near * far / (z * (far - near) + near);
}

float noise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Whether the sun reaches p. Everything counts as opaque: leaves are
// their cards here (alpha testing them is what mesh.frag's rays cost most).
bool sunlit(vec3 p, vec3 toSun)
{
    rayQueryEXT query;
    rayQueryInitializeEXT(query, rayScene, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, maskCasts, p,
                          0.0, toSun, 4096.0);
    rayQueryProceedEXT(query);
    return rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}

void main()
{
    vec3 eye = vec3(post.params.x, post.params.y, post.params3.w);
    vec3 toSun = vec3(post.extra[0].w, post.extra[1].w, post.extra[2].w);
    // Through this pixel, one stud along the view's forward per unit.
    vec3 ray = post.extra[0].xyz + (inUv.x * 2.0 - 1.0) * post.extra[1].xyz - (inUv.y * 2.0 - 1.0) * post.extra[2].xyz;
    float z = textureLod(depthTexture, inUv, 0.0).x;
    float perStud = length(ray);
    float distance = min(z <= 0.0 ? 1e9 : linearDepth(z) * perStud, post.params3.z);
    vec3 direction = ray / perStud;

    float stepLength = distance / float(steps);
    float t = stepLength * noise(gl_FragCoord.xy);
    float lit = 0.0;
    for (int i = 0; i < steps; ++i) {
        if (sunlit(eye + direction * t, toSun))
            lit += stepLength;
        t += stepLength;
    }
    // Henyey-Greenstein: how much of the sunlight turns towards the camera.
    float g = post.params.z;
    float phase = (1.0 - post.params.w)
                  / (12.5663706 * pow(max(1.0 + post.params.w - 2.0 * g * dot(direction, toSun), 1e-4), 1.5));
    outColor = vec4(post.params2.rgb * (lit * phase * post.params2.w), 1.0);
}
