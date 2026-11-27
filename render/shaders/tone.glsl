// The client's tone curve: what its final image pass (ImageProcessFS and
// every ImageProcess*FS variant in shaders_glsl3.pack) does to the scene's
// light x before ColorCorrection,
//     x (k x + b) / (x (k x + c) + d) * e
// with k, b, c, d in Params5 and e in Params6.x, copied there by
// sub_104976fbc. The values are one of two rows of a table in the 0.742
// macOS player (0x106766d20 and 0x106766d38), picked when the pass's
// settings are filled (0x104876c15) by a flag that 0x10491a444 sets: an
// enabled colour grading effect whose preset is 1 (TonemapperPreset.Retro),
// or else Lighting.Technology being Compatibility. GUESS: that the object
// it reads the preset from is a ColorGradingEffect (Lunarstone has none
// yet, so only Compatibility picks Retro here).
//
// The curve's output goes to the screen as it is: it is the client's gamma
// as well as its tone map. Retro tracks the forward shaders' plain
// sqrt(clamp(x)) to within a few percent, the old look; Default is darker
// below about 0.2 and rolls off softly above 1. The scene the pass reads
// holds light up to 4 (sqrt(x / 4) in 8 bits, which the pass squares and
// multiplies back), so that is the most either curve maps.
// GUESS: the client takes this pass at every quality Lunarstone draws, and
// its sqrt-only output is for when it skips the pass.
const vec4 toneCurveDefault = vec4(0.3, 0.4, 0.38, 0.12);
const float toneScaleDefault = 1.0;
const vec4 toneCurveRetro = vec4(0.88, 0.16, 1.34, 0.028);
const float toneScaleRetro = 2.18;
const float toneHeadroom = 4.0;

// Light to what the screen shows (sRGB-encoded, 0 to 1).
vec3 toneCurveEncoded(vec3 x, bool retro)
{
    vec4 curve = retro ? toneCurveRetro : toneCurveDefault;
    float scale = retro ? toneScaleRetro : toneScaleDefault;
    x = clamp(x, 0.0, toneHeadroom);
    vec3 kx = curve.x * x;
    return clamp(x * (kx + curve.y) / (x * (kx + curve.z) + curve.w) * scale, 0.0, 1.0);
}

// The tone mapped colour as the shaders write it: linear, for the sRGB
// target (or colorCorrect) to encode back to the curve's output.
vec3 tonemap(vec3 x, bool retro)
{
    vec3 y = toneCurveEncoded(x, retro);
    return mix(y / 12.92, pow((y + 0.055) / 1.055, vec3(2.4)), step(0.04045, y));
}

// tonemap's inverse: the light behind a written colour, up to the headroom.
vec3 untonemap(vec3 linear, bool retro)
{
    vec4 curve = retro ? toneCurveRetro : toneCurveDefault;
    float scale = retro ? toneScaleRetro : toneScaleDefault;
    vec3 c = clamp(linear, 0.0, 1.0);
    vec3 y = mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)) / scale;
    // y (x (k x + c) + d) = x (k x + b), a quadratic in x whose positive
    // root is taken in the form that holds where the x squared terms
    // cancel (Default's top).
    vec3 a = curve.x * (1.0 - y);
    vec3 b = curve.y - curve.z * y;
    vec3 dy = curve.w * y;
    vec3 root = 2.0 * dy / max(b + sqrt(max(b * b + 4.0 * a * dy, 0.0)), 1e-6);
    // White could have been any light from where the curve reaches 1 (just
    // under 1 for Retro) up: take it as the most there was.
    vec3 top = vec3(toneHeadroom);
    return mix(min(root, top), top, greaterThanEqual(c, vec3(1.0)));
}
