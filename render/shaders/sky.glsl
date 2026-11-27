// The sky and what it does to distant things, shared by the sky pass and the
// mesh shaders. Include after frame.glsl with GL_EXT_nonuniform_qualifier on.

vec4 sampleTexture(uint index, vec2 uv, uint s)
{
    return texture(sampler2D(textures[nonuniformEXT(index)], samplers[s]), uv);
}

vec3 skyDirection(vec3 d)
{
    // Sky.SkyboxOrientation.
    return vec3(dot(frame.skyRotation[0].xyz, d), dot(frame.skyRotation[1].xyz, d), dot(frame.skyRotation[2].xyz, d));
}

// Which Sky face a direction falls on and where, the way the client lays
// them out: as an OpenGL cube map with SkyboxRt on +X, Lf on -X, Up on +Y,
// Dn on -Y, Bk on +Z and Ft on -Z (G3D's "Quake" convention, which Roblox
// kept), with Up turned a quarter clockwise and Dn a quarter anticlockwise.
// Checked by the seams of the client's default sky512 faces, which only line
// up this way (or as its mirror image). Faces: 0 Rt, 1 Lf, 2 Up, 3 Dn, 4 Bk, 5 Ft.
uint skyFace(vec3 d, out vec2 uv)
{
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) {
        uv = vec2(0.5 - 0.5 * sign(d.x) * d.z / a.x, 0.5 - 0.5 * d.y / a.x);
        return d.x > 0.0 ? 0u : 1u;
    }
    if (a.z >= a.y) {
        uv = vec2(0.5 + 0.5 * sign(d.z) * d.x / a.z, 0.5 - 0.5 * d.y / a.z);
        return d.z > 0.0 ? 4u : 5u;
    }
    if (d.y > 0.0) {
        vec2 base = vec2(0.5 + 0.5 * d.x / a.y, 0.5 + 0.5 * d.z / a.y);
        uv = vec2(1.0 - base.y, base.x);
        return 2u;
    }
    vec2 base = vec2(0.5 + 0.5 * d.x / a.y, 0.5 - 0.5 * d.z / a.y);
    uv = vec2(base.y, 1.0 - base.x);
    return 3u;
}

uint skyFaceTexture(uint face)
{
    return face < 4u ? frame.skyFaces0[face] : frame.skyFaces1[face - 4u];
}

// The skybox as drawn: its texture times the time of day's tint.
vec3 skyboxColor(vec3 d)
{
    vec2 uv;
    vec3 r = skyDirection(d);
    uint face = skyFace(r, uv);
    uint index = skyFaceTexture(face);
    if (index == 0u)
        return frame.sky.rgb * frame.skyTint.rgb;
    // Mip level from how fast the direction turns across the pixel, which
    // unlike uv doesn't jump where two faces meet (no seams).
    float major = max(abs(r.x), max(abs(r.y), abs(r.z)));
    float step = 0.5 * length(fwidth(r)) / major;
    vec3 color = textureGrad(sampler2D(textures[nonuniformEXT(index)], samplers[1]), uv, vec2(step, 0.0),
                             vec2(0.0, step))
                     .rgb;
    return color * frame.skyTint.rgb;
}

// The skybox blurred down to a colour per direction (each face's mean,
// blended like an ambient cube): what fog fades into, the way the client
// fades into its prefiltered environment map at its blurriest level.
vec3 skyboxBlurred(vec3 d)
{
    vec3 r = skyDirection(d);
    vec3 n = r * r;
    vec3 color = n.x * (r.x > 0.0 ? frame.skyAverage[0].rgb : frame.skyAverage[1].rgb)
                 + n.y * (r.y > 0.0 ? frame.skyAverage[2].rgb : frame.skyAverage[3].rgb)
                 + n.z * (r.z > 0.0 ? frame.skyAverage[4].rgb : frame.skyAverage[5].rgb);
    return color * frame.skyTint.rgb;
}

// Atmosphere. A guess at the look, not the client's AdvSky shader (not in the
// GLSL pack): Color towards the sun and Decay away from it, thicker towards
// the horizon, Offset hiding more of the sky behind it, Haze greying and
// raising it, Glare a glow around the sun.
vec3 atmosphereTint(vec3 d)
{
    float towardsSun = 0.5 + 0.5 * dot(d, frame.sunDirection.xyz);
    vec3 color = mix(frame.atmosphereDecay.rgb, frame.atmosphereColor.rgb, towardsSun);
    float haze = clamp(frame.atmosphereParams.y / 10.0, 0.0, 1.0);
    color = mix(color, vec3(dot(color, vec3(0.3333))) * 1.1, haze * 0.5);
    float glare = frame.atmosphereParams.x * pow(max(dot(d, frame.sunDirection.xyz), 0.0), 12.0);
    color += frame.sunColor.rgb * glare * 0.25;
    return color * frame.skyTint.rgb;
}

// How much of the sky in direction d the atmosphere covers.
float atmosphereCover(vec3 d)
{
    float density = frame.atmosphereColor.w;
    if (density <= 0.0)
        return 0.0;
    float haze = clamp(frame.atmosphereParams.y / 10.0, 0.0, 1.0);
    // Air this many studs thick overhead, longer towards the horizon.
    float height = 300.0 * (1.0 + 4.0 * haze);
    float path = height / max(d.y + 0.05, 0.05);
    float cover = 1.0 - exp(-density * path);
    return mix(cover, 1.0, clamp(frame.atmosphereDecay.w, 0.0, 1.0) * smoothstep(0.6, 0.0, d.y));
}

vec3 applyAtmosphereToSky(vec3 color, vec3 d)
{
    return mix(color, atmosphereTint(d), atmosphereCover(d));
}

// What distant geometry seen along d fades into.
vec3 distanceColor(vec3 d)
{
    vec3 flat_d = normalize(vec3(d.x, min(d.y, 0.2), d.z));
    return applyAtmosphereToSky(skyboxBlurred(flat_d), flat_d);
}

// Linear colour to sRGB-encoded and back, the target's own curve.
vec3 srgbEncode(vec3 linear)
{
    vec3 c = clamp(linear, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 srgbDecode(vec3 srgb)
{
    return mix(srgb / 12.92, pow((srgb + 0.055) / 1.055, vec3(2.4)), step(0.04045, srgb));
}

// Lighting's ColorCorrection effects on an sRGB-encoded colour, the space
// the client's post pass applies them in (after tone mapping): a shader
// that has its colour encoded already (tone.glsl's toneCurveEncoded) saves
// the trip to linear and back.
vec3 colorCorrectEncoded(vec3 srgb)
{
    vec4 x = vec4(srgb, 1.0);
    return clamp(vec3(dot(frame.colorCorrection[0], x), dot(frame.colorCorrection[1], x),
                      dot(frame.colorCorrection[2], x)),
                 0.0, 1.0);
}

// The same on a final linear colour.
vec3 colorCorrect(vec3 linear)
{
    return srgbDecode(colorCorrectEncoded(srgbEncode(linear)));
}
