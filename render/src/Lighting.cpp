#include <lunar/render/Lighting.hpp>

#include <lunar/Classes.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

namespace lunar::render {

namespace {

Color3 mix(Color3 a, Color3 b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

Color3 scaled(Color3 c, float s)
{
    return {c.r * s, c.g * s, c.b * s};
}

// Lighting's Sky. A Sky's property defaults are the client's default sky,
// which is also what shows without one.
const Sky& skyOf(const lunar::Lighting& service)
{
    if (const auto* sky = static_cast<const Sky*>(service.findFirstChildOfClass("Sky")))
        return *sky;
    static const std::shared_ptr<const Sky> defaults = std::make_shared<Sky>();
    return *defaults;
}

} // namespace

namespace {

// The client's sky model (RBX's take on G3D's SkyParameters::setTime): the
// day's turn and colour keys by the second since midnight.
constexpr double secondsPerDay = 86400.0;

double secondOfDay(double clockTime)
{
    const double seconds = std::fmod(clockTime * 3600.0, secondsPerDay);
    return seconds < 0.0 ? seconds + secondsPerDay : seconds;
}

// A body a half turn into the day from `phase` (0 for the sun, pi for the
// moon): (sin a, -cos a, 0) turned by the latitude about the horizontal axis
// across its path, which leans the path towards +Z. The tilt also carries a
// season term the way the client has it, though with the time of day alone
// it stays under half a degree.
Vector3 celestialBody(double clockTime, float latitude, float phase)
{
    const double seconds = secondOfDay(clockTime);
    const float turn = static_cast<float>(seconds) * 2.0f * std::numbers::pi_v<float> / 86400.0f + phase;
    const double days = seconds / secondsPerDay;
    const auto season
        = static_cast<float>(std::sin((days - 182.6282) * std::numbers::pi / 182.6282) * -0.41015237421866746);
    const float tilt = latitude * std::numbers::pi_v<float> / -180.0f + season;
    // Rodrigues about (cos a, sin a, 0), which is square to the body.
    return {std::sin(turn) * std::cos(tilt), -std::cos(turn) * std::cos(tilt), -std::sin(tilt)};
}

// Colour keys over the day, interpolated linearly the way the client's
// keyframe lookup does; past the ends they hold.
template <std::size_t N>
Color3 keyed(const std::array<double, N>& hours, const std::array<Color3, N>& keys, double seconds)
{
    if (seconds <= hours[0] * 3600.0)
        return keys[0];
    for (std::size_t i = 1; i < N; ++i) {
        const double at = hours[i] * 3600.0;
        if (at > seconds) {
            const double before = hours[i - 1] * 3600.0;
            const auto t = static_cast<float>((at - seconds) / (at - before));
            return mix(keys[i], keys[i - 1], t);
        }
    }
    return keys[N - 1];
}

// The sun's colour (squared later, like every client colour); the moon's
// light at night is this too.
Color3 sunKeyColor(double seconds)
{
    static constexpr std::array<double, 10> hours{0.0, 5.0, 6.0, 6.125, 6.5, 17.5, 17.75, 18.0, 18.5, 24.0};
    static constexpr std::array<Color3, 10> keys{{
        {0.2f, 0.2f, 0.2f},
        {0.1f, 0.1f, 0.1f},
        {0.0f, 0.0f, 0.0f},
        {0.6f, 0.6f, 0.0f},
        {0.75f, 0.75f, 0.75f},
        {0.75f, 0.75f, 0.75f},
        {0.1f, 0.1f, 0.075f},
        {0.1f, 0.05f, 0.05f},
        {0.1f, 0.1f, 0.1f},
        {0.2f, 0.2f, 0.2f},
    }};
    return keyed(hours, keys, seconds);
}

// The sky gradient over the day, top and bottom, whose night end follows
// EnvironmentDiffuseScale.
Color3 gradientTop(double seconds, float environment)
{
    const float e = 0.2f * environment;
    const float h = 0.5f * e;
    static constexpr std::array<double, 10> hours{0.0, 4.0, 5.0, 5.5, 6.0, 6.5, 17.5, 18.0, 18.0 + 1.0 / 3.0, 24.0};
    const std::array<Color3, 10> keys{{
        {e, e, e},
        {h, h, h},
        {0.07f + h, 0.07f + h, 0.1f + h},
        {0.2f + h, 0.15f + h, 0.01f + h},
        {0.2f + h, 0.15f + h, 0.01f + h},
        {1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {0.4f + h, 0.2f + h, 0.05f + h},
        {h, h, h},
        {e, e, e},
    }};
    return keyed(hours, keys, seconds);
}

Color3 gradientBottom(double seconds, float environment)
{
    const float e = 0.2f * environment;
    const float h = 0.5f * e;
    static constexpr std::array<double, 12> hours{0.0,  3.0,  4.0, 5.5, 6.0, 6.5, 17.5, 18.0, 18.0 + 1.0 / 3.0,
                                                  20.0, 21.0, 24.0};
    const std::array<Color3, 12> keys{{
        {h, h, h},
        {e, e, e},
        {0.21f, 0.21f, 0.28f},
        {0.4f, 0.3f, 0.3f},
        {0.3f, 0.2f, 0.3f},
        {1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {0.4f, 0.3f, 0.2f},
        {0.3f, 0.2f, 0.3f},
        {0.3f, 0.2f, 0.3f},
        {e, e, e},
        {h, h, h},
    }};
    return keyed(hours, keys, seconds);
}

Color3 squared(Color3 c)
{
    return {c.r * c.r, c.g * c.g, c.b * c.b};
}

float length(Color3 c)
{
    return std::sqrt(c.r * c.r + c.g * c.g + c.b * c.b);
}

// How the client tints a lamp by a ColorShift (all colours squared): light
// keeps its strength, pushed towards the shift's hue (`colored`), or with
// the skybox lighting things (EnvironmentDiffuseScale 1) it becomes the
// shift itself, scaled by the light and by how coloured the shift is.
// Black shifts change nothing at 0 and give `fallback` at 1.
Color3 shifted(Color3 light, Color3 fallback, Color3 shift, float environment)
{
    const float mean = (shift.r + shift.g + shift.b) * 0.3333f;
    const Color3 hue{shift.r - mean, shift.g - mean, shift.b - mean};
    const float strength = length(light);
    const Color3 colored{light.r + strength * hue.r, light.g + strength * hue.g, light.b + strength * hue.b};
    const float amount = length(hue);
    const Color3 environmentLit{fallback.r + amount * (shift.r * strength - fallback.r),
                                fallback.g + amount * (shift.g * strength - fallback.g),
                                fallback.b + amount * (shift.b * strength - fallback.b)};
    return mix(colored, environmentLit, environment);
}

} // namespace

Vector3 sunDirection(double clockTime, float latitude)
{
    return celestialBody(clockTime, latitude, 0.0f);
}

Vector3 moonDirection(double clockTime, float latitude)
{
    return celestialBody(clockTime, latitude, std::numbers::pi_v<float>);
}

SkyTextureIds skyTextureIds(const lunar::Lighting& service)
{
    const Sky& s = skyOf(service);
    return {
        .faces = {s.skyboxRt(), s.skyboxLf(), s.skyboxUp(), s.skyboxDn(), s.skyboxBk(), s.skyboxFt()},
        .sun = s.sunTextureId(),
        .moon = s.moonTextureId(),
    };
}

Lighting lightingFrom(const lunar::Lighting& service, const TextureProvider& textures)
{
    Lighting lighting;
    lighting.technology = service.technology();
    const double clock = service.clockTime();
    const double seconds = secondOfDay(clock);
    const Vector3 sun = sunDirection(clock, service.geographicLatitude());
    const Vector3 moon = moonDirection(clock, service.geographicLatitude());
    lighting.sunPosition = sun;
    lighting.moonDirection = moon;
    // The client lights from the sun until it's well below the horizon.
    lighting.sunDirection = sun.y > -0.3f ? sun : moon;

    // 0 at night, 1 by day, by the clock like the client's sky keys (the sun
    // itself can stay low or even under the horizon all day at high
    // GeographicLatitude); warm around sunrise and sunset.
    const Color3 gradient = gradientTop(seconds, 0.0f);
    const float day = std::clamp((gradient.r + gradient.g + gradient.b) / 3.0f, 0.0f, 1.0f);
    const double hour = seconds / 3600.0;
    const float low
        = static_cast<float>(std::clamp(1.0 - std::min(std::abs(hour - 6.25), std::abs(hour - 17.75)) / 1.5, 0.0, 1.0));

    const Color3 daySky = Color3::fromRGB(150, 196, 236);
    const Color3 sunsetSky = Color3::fromRGB(232, 162, 118);
    const Color3 nightSky = Color3::fromRGB(14, 18, 34);
    lighting.sky = mix(nightSky, mix(daySky, sunsetSky, low * 0.55f), day);

    // The client's lamps, ambient and sky gradient (the function that fills
    // its Globals from Lighting, sub_104915ff4 in the 0.742 macOS player,
    // and the sky model's keys, sub_1031b95ea). Colours are squared to
    // linear.
    const float environment = std::clamp(service.environmentDiffuseScale(), 0.0f, 1.0f);
    lighting.environmentDiffuse = environment;
    lighting.environmentSpecular = std::clamp(service.environmentSpecularScale(), 0.0f, 1.0f);
    const float brightness = std::max(service.brightness(), 0.0f);
    Color3 sunColor = sunKeyColor(seconds);
    const float floor = 0.35f * environment;
    sunColor = {std::max(sunColor.r, floor), std::max(sunColor.g, floor), std::max(sunColor.b, floor)};
    const Color3 sunLight = squared(sunColor);
    lighting.sunColor
        = scaled(shifted(sunLight, sunLight, squared(service.colorShiftTop()), environment), 0.9f * brightness);
    lighting.backColor
        = scaled(shifted(sunLight, {}, squared(service.colorShiftBottom()), environment), 0.36f * brightness);
    const Color3 ambient = squared(service.ambient());
    const Color3 outdoor = squared(service.outdoorAmbient());
    lighting.ambient = ambient;
    lighting.skyAmbient = {std::max(outdoor.r - ambient.r, 0.0f), std::max(outdoor.g - ambient.g, 0.0f),
                           std::max(outdoor.b - ambient.b, 0.0f)};
    lighting.skyGradientTop = squared(gradientTop(seconds, environment));
    lighting.skyGradientBottom = squared(gradientBottom(seconds, environment));
    // GUESS: the client's exposure is 2^ExposureCompensation.
    lighting.exposure = std::exp2(std::clamp(service.exposureCompensation(), -5.0f, 5.0f));
    // The client's flag for its Retro tone curve, unless a colour grading
    // effect asks for it (0x10491a444; Lunarstone has no ColorGradingEffect).
    lighting.retroTone = service.technology() == Enum::Technology::Compatibility;

    lighting.shadows = service.globalShadows();
    lighting.shadowSoftness = std::clamp(service.shadowSoftness(), 0.0f, 1.0f);
    lighting.fogColor = service.fogColor();
    lighting.fogStart = service.fogStart();
    lighting.fogEnd = service.fogEnd();

    // An Atmosphere replaces Fog* with haze (drawn by the sky shader).
    if (const auto* atmosphere = static_cast<const Atmosphere*>(service.findFirstChildOfClass("Atmosphere"))) {
        lighting.fogEnd = 100000.0f;
        lighting.atmosphere = {
            .enabled = true,
            .color = atmosphere->color(),
            .decay = atmosphere->decay(),
            .density = std::clamp(atmosphere->density(), 0.0f, 1.0f),
            .offset = std::clamp(atmosphere->offset(), 0.0f, 1.0f),
            .glare = std::clamp(atmosphere->glare(), 0.0f, 10.0f),
            .haze = std::clamp(atmosphere->haze(), 0.0f, 10.0f),
        };
    }
    if (day < 1.0f)
        lighting.fogColor = mix(nightSky, lighting.fogColor, day);

    // The skybox darkens towards night and warms a little at sunrise and
    // sunset; stars come out as it gets dark. A guess at the client's sky
    // vertex colour (SkyLinearVColorFS multiplies the faces by it), which is
    // worked out on the CPU and not in the shader pack.
    const Color3 dusk = mix({1.0f, 1.0f, 1.0f}, {1.0f, 0.82f, 0.68f}, low * 0.5f);
    lighting.skyTint = mix({0.05f, 0.06f, 0.11f}, dusk, day);
    lighting.starBrightness = 1.0f - std::clamp(day * 4.0f, 0.0f, 1.0f);

    addPostEffects(lighting, service);

    const Sky& s = skyOf(service);
    lighting.skybox.sunAngularSize = s.sunAngularSize();
    lighting.skybox.moonAngularSize = s.moonAngularSize();
    lighting.skybox.starCount = std::clamp(s.starCount(), 0, 5000);
    lighting.skybox.celestialBodiesShown = s.celestialBodiesShown();
    lighting.skybox.orientation = s.skyboxOrientation();
    if (textures) {
        const SkyTextureIds ids = skyTextureIds(service);
        for (std::size_t i = 0; i < ids.faces.size(); ++i)
            lighting.skybox.faces[i] = ids.faces[i].empty() ? nullptr : textures(ids.faces[i]);
        // A sky with a face missing would look broken; the flat colour is better.
        if (std::ranges::any_of(lighting.skybox.faces, [](const ImagePtr& face) { return !face; }))
            lighting.skybox.faces = {};
        lighting.skybox.sun = ids.sun.empty() ? nullptr : textures(ids.sun);
        lighting.skybox.moon = ids.moon.empty() ? nullptr : textures(ids.moon);
    }
    return lighting;
}

void addPostEffects(Lighting& lighting, const Instance& parent)
{
    // ColorCorrection: the client applies them after tone mapping as one
    // 3x4 matrix (CB1[2..4] of ImageProcessFS). How the four properties
    // make it is worked out on the CPU; GUESS: contrast about mid grey,
    // then brightness added, saturation away from Rec. 709 luma, then the
    // tint, each effect after the last.
    for (const auto& child : parent.children()) {
        const auto* post = instanceCast<const PostEffect>(child.get());
        if (!post || !post->enabled())
            continue;
        PostLook& look = lighting.post;
        if (const auto* bloom = instanceCast<const BloomEffect>(post)) {
            if (!look.bloom || bloom->intensity() > look.bloomIntensity) {
                look.bloom = true;
                look.bloomIntensity = std::max(bloom->intensity(), 0.0f);
                look.bloomSize = std::clamp(bloom->size(), 0.0f, 56.0f);
                look.bloomThreshold = std::max(bloom->threshold(), 0.0f);
            }
            continue;
        }
        if (const auto* blur = instanceCast<const BlurEffect>(post)) {
            look.blur = std::max(look.blur, std::clamp(blur->size(), 0.0f, 56.0f));
            continue;
        }
        if (const auto* rays = instanceCast<const SunRaysEffect>(post)) {
            if (!look.sunRays || rays->intensity() > look.sunRaysIntensity) {
                look.sunRays = true;
                look.sunRaysIntensity = std::clamp(rays->intensity(), 0.0f, 1.0f);
                look.sunRaysSpread = std::clamp(rays->spread(), 0.0f, 1.0f);
            }
            continue;
        }
        if (const auto* dof = instanceCast<const DepthOfFieldEffect>(post)) {
            if (!look.depthOfField) {
                look.depthOfField = true;
                look.nearIntensity = std::clamp(dof->nearIntensity(), 0.0f, 1.0f);
                look.farIntensity = std::clamp(dof->farIntensity(), 0.0f, 1.0f);
                look.focusDistance = std::clamp(dof->focusDistance(), 0.0f, 200.0f);
                look.inFocusRadius = std::clamp(dof->inFocusRadius(), 0.0f, 50.0f);
            }
            continue;
        }
        const auto* effect = instanceCast<const ColorCorrectionEffect>(post);
        if (!effect)
            continue;
        using Matrix = std::array<std::array<float, 4>, 3>;
        auto times = [](const Matrix& a, const Matrix& b) {
            Matrix out{};
            for (std::size_t r = 0; r < 3; ++r)
                for (std::size_t c = 0; c < 4; ++c)
                    out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + (c == 3 ? a[r][3] : 0.0f);
            return out;
        };
        const float contrast = 1.0f + effect->contrast();
        const float offset = 0.5f * (1.0f - contrast) + effect->brightness();
        const Matrix tone{{
            {contrast, 0.0f, 0.0f, offset},
            {0.0f, contrast, 0.0f, offset},
            {0.0f, 0.0f, contrast, offset},
        }};
        const float saturation = 1.0f + effect->saturation();
        const std::array<float, 3> luma{0.2126f, 0.7152f, 0.0722f};
        Matrix saturate{};
        for (std::size_t r = 0; r < 3; ++r)
            for (std::size_t c = 0; c < 3; ++c)
                saturate[r][c] = luma[c] * (1.0f - saturation) + (r == c ? saturation : 0.0f);
        const Color3 tint = effect->tintColor();
        const Matrix tinted{{
            {tint.r, 0.0f, 0.0f, 0.0f},
            {0.0f, tint.g, 0.0f, 0.0f},
            {0.0f, 0.0f, tint.b, 0.0f},
        }};
        lighting.colorCorrection = times(tinted, times(saturate, times(tone, lighting.colorCorrection)));
    }
}

Lighting lightingFrom(const DataModel& game, const TextureProvider& textures)
{
    Lighting lighting;
    if (const auto* service = static_cast<const lunar::Lighting*>(game.findService("Lighting")))
        lighting = lightingFrom(*service, textures);
    // Terrain's Clouds, drawn in the sky.
    if (const Instance* workspace = game.findService("Workspace"))
        if (const Instance* terrain = workspace->findFirstChildOfClass("Terrain"))
            if (const auto* clouds = instanceCast<const Clouds>(terrain->findFirstChildOfClass("Clouds"))) {
                lighting.clouds = {
                    .enabled = clouds->enabled(),
                    .cover = std::clamp(clouds->cover(), 0.0f, 1.0f),
                    .density = std::clamp(clouds->density(), 0.0f, 1.0f),
                    .color = clouds->color(),
                };
            }
    // Post effects under the camera count as well as Lighting's.
    if (const auto* workspace = instanceCast<const Workspace>(game.findService("Workspace")))
        if (const Instance* camera = workspace->currentCamera())
            addPostEffects(lighting, *camera);
    return lighting;
}

} // namespace lunar::render
