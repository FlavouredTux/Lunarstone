#include "MusicVisualizer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>

namespace {

constexpr float pi = std::numbers::pi_v<float>;
// A beat's ring: how long it lives and how far it gets past the pillars.
constexpr float waveSeconds = 0.8f;
constexpr float waveReach = 12.0f;
// A cap falls like a dropped thing, from rest.
constexpr float capGravity = 9.0f;
constexpr float capThickness = 0.18f;

using Rgb = std::array<float, 3>;

// A rainbow's colour at `hue` (0..1 round the wheel), full and bright.
Rgb rainbow(float hue)
{
    const float h = (hue - std::floor(hue)) * 6.0f;
    const float x = 1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f);
    switch (static_cast<int>(h)) {
    case 0: return {1.0f, x, 0.0f};
    case 1: return {x, 1.0f, 0.0f};
    case 2: return {0.0f, 1.0f, x};
    case 3: return {0.0f, x, 1.0f};
    case 4: return {x, 0.0f, 1.0f};
    default: return {1.0f, 0.0f, x};
    }
}

Rgb scaled(Rgb c, float k)
{
    return {c[0] * k, c[1] * k, c[2] * k};
}

Rgb towardWhite(Rgb c, float t)
{
    return {c[0] + (1.0f - c[0]) * t, c[1] + (1.0f - c[1]) * t, c[2] + (1.0f - c[2]) * t};
}

// Appends vertices and triangles to one EffectGeometry.
struct Mesh {
    lunar::render::EffectGeometry& out;

    std::uint32_t vertex(lunar::Vector3 at, Rgb rgb, float alpha) const
    {
        lunar::render::EffectVertex v;
        v.position = at;
        v.color[0] = rgb[0];
        v.color[1] = rgb[1];
        v.color[2] = rgb[2];
        v.color[3] = alpha;
        out.vertices.push_back(v);
        return static_cast<std::uint32_t>(out.vertices.size() - 1);
    }
    void quad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) const
    {
        out.indices.insert(out.indices.end(), {a, b, c, a, c, d});
    }
    // A flat band round `center` from `inner` to `outer` studs out, rainbow
    // round it (turned by `turn`), its opacity `innerAlpha` at the inside
    // and `outerAlpha` at the outside.
    void band(lunar::Vector3 center, float inner, float outer, float innerAlpha, float outerAlpha, float turn) const
    {
        constexpr int segments = 192;
        for (int i = 0; i < segments; ++i) {
            const float a0 = static_cast<float>(i) / segments, a1 = static_cast<float>(i + 1) / segments;
            const float r0 = 2.0f * pi * (a0 + turn), r1 = 2.0f * pi * (a1 + turn);
            const lunar::Vector3 d0{std::cos(r0), 0.0f, std::sin(r0)}, d1{std::cos(r1), 0.0f, std::sin(r1)};
            const Rgb c0 = rainbow(a0), c1 = rainbow(a1);
            quad(vertex(center + d0 * inner, c0, innerAlpha), vertex(center + d0 * outer, c0, outerAlpha),
                 vertex(center + d1 * outer, c1, outerAlpha), vertex(center + d1 * inner, c1, innerAlpha));
        }
    }
};

} // namespace

MusicVisualizer::MusicVisualizer(lunar::render::MeshRenderer& renderer) : renderer_(renderer)
{
    renderer_.addEffects(this);
}

MusicVisualizer::~MusicVisualizer()
{
    renderer_.removeEffects(this);
}

void MusicVisualizer::setEnabled(bool on)
{
    if (on == static_cast<bool>(capture_))
        return;
    capture_ = on ? std::make_unique<lunar::audio::MonitorCapture>() : nullptr;
}

float MusicVisualizer::barHeight(int i) const
{
    // Mirrored: the bass on two sides, the treble between them.
    const std::span<const float> bands = spectrum_.bands();
    const auto band = static_cast<std::size_t>(i < bars / 2 ? i : bars - 1 - i);
    const float level = band < bands.size() ? bands[band] : 0.0f;
    return 0.2f + std::pow(level, 1.5f) * tallest + spectrum_.beat() * 0.5f;
}

void MusicVisualizer::update(std::optional<lunar::Vector3> feet, float dt)
{
    feet_ = feet;
    if (!capture_)
        return;
    spectrum_.push(capture_->take());
    spectrum_.update(dt);
    turn_ = std::fmod(turn_ + static_cast<double>(dt) / static_cast<double>(turnSeconds), 1.0);
    // Caps: pushed up by their pillar, else falling.
    for (int i = 0; i < bars; ++i) {
        const float height = barHeight(i);
        auto& peak = peaks_[static_cast<std::size_t>(i)];
        auto& fall = peakFall_[static_cast<std::size_t>(i)];
        if (height >= peak) {
            peak = height;
            fall = 0.0f;
        } else {
            fall += capGravity * dt;
            peak = std::max(height, peak - fall * dt);
        }
    }
    // A beat sends a ring out.
    for (float& age : waves_)
        age += dt;
    std::erase_if(waves_, [](float age) { return age > waveSeconds; });
    const float beat = spectrum_.beat();
    if (beat > 0.95f && lastBeat_ < 0.9f)
        waves_.push_back(0.0f);
    lastBeat_ = beat;
}

std::string MusicVisualizer::status() const
{
    if (!capture_)
        return {};
    if (std::string error = capture_->error(); !error.empty())
        return "Can't listen: " + error;
    return spectrum_.level() > 0.02f ? "Listening to what your speakers play"
                                     : "Listening, but nothing is playing (any player works: Spotify, a browser...)";
}

void MusicVisualizer::collect(const lunar::render::EffectView& view, lunar::render::EffectGeometry& out)
{
    if (!capture_ || !feet_)
        return;
    const lunar::Vector3 base = *feet_ + lunar::Vector3{0.0f, 0.03f, 0.0f};
    const lunar::Vector3 eye = view.camera.cframe.position;
    const float beat = spectrum_.beat();
    const auto turn = static_cast<float>(turn_);
    const Mesh mesh{out};

    // The pillars, solid: the effect pass writes no depth, so they go far
    // to near, each with only the faces turned to the camera. Angles grow
    // from +X towards +Z, clockwise seen from above.
    struct Pillar {
        int index;
        float distance;
    };
    std::array<Pillar, bars> order{};
    auto radial = [&](int i) {
        const float a = 2.0f * pi * (static_cast<float>(i) / bars + turn);
        return lunar::Vector3{std::cos(a), 0.0f, std::sin(a)};
    };
    for (int i = 0; i < bars; ++i)
        order[static_cast<std::size_t>(i)] = {i, (base + radial(i) * radius - eye).magnitude()};
    std::ranges::sort(order, std::greater{}, &Pillar::distance);

    const float spacing = 2.0f * pi * radius / bars;
    const float halfWide = spacing * 0.32f, halfDeep = spacing * 0.32f;
    auto first = static_cast<std::uint32_t>(out.indices.size());
    // A box from `bottom` to `top` studs up at bar `i`: faces the camera
    // sees, lit by how squarely they face it, darker towards the ground.
    auto box = [&](int i, float bottom, float top, Rgb rgb, float whiteTop, float bottomShade) {
        const lunar::Vector3 outward = radial(i);
        const lunar::Vector3 along{-outward.z, 0.0f, outward.x};
        const lunar::Vector3 foot = base + outward * radius;
        auto corner = [&](float u, float v, float y) {
            return foot + along * (u * halfWide) + outward * (v * halfDeep) + lunar::Vector3{0.0f, y, 0.0f};
        };
        auto face = [&](lunar::Vector3 normal, std::array<lunar::Vector3, 4> corners, bool isTop) {
            const lunar::Vector3 middle = (corners[0] + corners[2]) * 0.5f;
            const lunar::Vector3 toEye = eye - middle;
            if (normal.dot(toEye) <= 0.0f)
                return;
            const float facing = std::max(normal.dot(toEye.unit()), 0.0f);
            const float light = isTop ? 1.0f : 0.55f + 0.45f * facing;
            const Rgb lit = isTop ? towardWhite(rgb, whiteTop) : scaled(rgb, light);
            const Rgb low = isTop ? lit : scaled(lit, bottomShade);
            mesh.quad(mesh.vertex(corners[0], low, 1.0f), mesh.vertex(corners[1], low, 1.0f),
                      mesh.vertex(corners[2], lit, 1.0f), mesh.vertex(corners[3], lit, 1.0f));
        };
        face(outward, {corner(-1, 1, bottom), corner(1, 1, bottom), corner(1, 1, top), corner(-1, 1, top)}, false);
        face(outward * -1.0f, {corner(1, -1, bottom), corner(-1, -1, bottom), corner(-1, -1, top), corner(1, -1, top)},
             false);
        face(along, {corner(1, 1, bottom), corner(1, -1, bottom), corner(1, -1, top), corner(1, 1, top)}, false);
        face(along * -1.0f, {corner(-1, -1, bottom), corner(-1, 1, bottom), corner(-1, 1, top), corner(-1, -1, top)},
             false);
        face({0.0f, 1.0f, 0.0f}, {corner(-1, -1, top), corner(1, -1, top), corner(1, 1, top), corner(-1, 1, top)},
             true);
    };
    for (const Pillar& p : order) {
        const Rgb rgb = rainbow(static_cast<float>(p.index) / bars);
        const float height = barHeight(p.index);
        box(p.index, 0.0f, height, rgb, 0.45f, 0.4f);
        // Its cap, floating at the peak while the pillar drops away.
        const float peak = peaks_[static_cast<std::size_t>(p.index)] + 0.15f;
        box(p.index, peak, peak + capThickness, towardWhite(rgb, 0.6f), 0.8f, 1.0f);
    }
    lunar::render::EffectBatch solid;
    solid.firstIndex = first;
    solid.indexCount = static_cast<std::uint32_t>(out.indices.size()) - first;
    solid.lightEmission = 0.0f; // drawn over what's behind, like a solid thing
    solid.lightInfluence = 0.0f;
    solid.brightness = 1.4f;
    solid.center = base + lunar::Vector3{0.0f, 2.0f, 0.0f};
    solid.radius = radius + tallest;
    out.batches.push_back(solid);

    // The light they give: a glow on the ground under the ring, brighter on
    // a beat, and each beat's ring rushing out and fading.
    first = static_cast<std::uint32_t>(out.indices.size());
    const float glow = 0.3f + beat * 0.5f;
    mesh.band(base, radius - 2.0f, radius, 0.0f, glow, turn);
    mesh.band(base, radius, radius + 2.5f, glow, 0.0f, turn);
    for (float age : waves_) {
        const float t = age / waveSeconds;
        const float at = radius + 1.0f + t * waveReach;
        const float fade = (1.0f - t) * 0.9f;
        mesh.band(base, at - 0.35f, at, 0.0f, fade, turn);
        mesh.band(base, at, at + 0.15f, fade, 0.0f, turn);
    }
    lunar::render::EffectBatch light;
    light.firstIndex = first;
    light.indexCount = static_cast<std::uint32_t>(out.indices.size()) - first;
    light.lightEmission = 1.0f; // added to what's behind: a glow
    light.lightInfluence = 0.0f;
    light.brightness = 1.5f;
    light.center = base;
    light.radius = radius + waveReach + 2.0f;
    out.batches.push_back(light);
}
