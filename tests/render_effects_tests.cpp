#include "render_gpu.hpp"

#include <lunar/Classes.hpp>
#include <lunar/render/Adornments.hpp>
#include <lunar/render/EffectGeometry.hpp>
#include <lunar/render/Graphics.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/render/Ribbons.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <numbers>
#include <print>
#include <vector>

// Not all of lunar: the engine's Camera and Lighting classes would clash
// with the renderer's.
using lunar::CFrame;
using lunar::Color3;
using lunar::Vector3;
using namespace lunar::render;

namespace {

constexpr std::uint32_t width = 64;
constexpr std::uint32_t height = 48;

std::unique_ptr<OffscreenRenderer> makeOffscreen(Context& context, std::uint32_t w = width, std::uint32_t h = height)
{
    auto offscreen = OffscreenRenderer::create(context, w, h);
    if (!offscreen)
        throw lt::Failure{offscreen.error()};
    return std::move(*offscreen);
}

Camera frontCamera()
{
    Camera camera;
    camera.cframe = CFrame(0.0f, 0.0f, 10.0f);
    return camera;
}

DrawItem box(Vector3 position, Vector3 size, Color3 color, float transparency = 0.0f)
{
    return {partMatrix(CFrame(position), size), color, transparency};
}

// The same effect batches every frame.
struct FixedEffects : EffectSource {
    EffectGeometry geometry;
    int frames = 0;

    void collect(const EffectView&, EffectGeometry& out) override
    {
        ++frames;
        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        const auto firstIndex = static_cast<std::uint32_t>(out.indices.size());
        out.vertices.insert(out.vertices.end(), geometry.vertices.begin(), geometry.vertices.end());
        for (std::uint32_t index : geometry.indices)
            out.indices.push_back(base + index);
        for (EffectBatch batch : geometry.batches) {
            batch.firstIndex += firstIndex;
            out.batches.push_back(batch);
        }
    }

    // A square facing +Z at z, `size` studs on a side.
    EffectBatch& square(float z, float size, Color3 color, float opacity, float lightEmission = 0.0f)
    {
        const auto first = static_cast<std::uint32_t>(geometry.vertices.size());
        const float h = size * 0.5f;
        for (auto [x, y, u, v] : {std::array{-h, h, 0.0f, 0.0f}, std::array{h, h, 1.0f, 0.0f},
                                  std::array{-h, -h, 0.0f, 1.0f}, std::array{h, -h, 1.0f, 1.0f}}) {
            EffectVertex& vertex = geometry.vertices.emplace_back();
            vertex.position = {x, y, z};
            vertex.uvq[0] = u;
            vertex.uvq[1] = v;
            vertex.color[0] = color.r;
            vertex.color[1] = color.g;
            vertex.color[2] = color.b;
            vertex.color[3] = opacity;
        }
        EffectBatch& batch = geometry.batches.emplace_back();
        batch.firstIndex = static_cast<std::uint32_t>(geometry.indices.size());
        batch.indexCount = 6;
        for (std::uint32_t i : {0u, 2u, 1u, 1u, 2u, 3u})
            geometry.indices.push_back(first + i);
        batch.lightEmission = lightEmission;
        batch.center = {0.0f, 0.0f, z};
        batch.radius = h * 1.5f;
        return batch;
    }
};

Rgba centre(const PixelImage& image)
{
    return image.at(image.width / 2, image.height / 2);
}

bool reddish(Rgba p)
{
    return p.r >= 180 && p.g <= 40 && p.b <= 40;
}

} // namespace

GPU_TEST(effects_blend_normally_or_add_by_light_emission)
{
    auto offscreen = makeOffscreen(context);
    FixedEffects effects;
    offscreen->renderer().addEffects(&effects);
    const Rgba sky = centre(offscreen->render(frontCamera(), {}));
    CHECK_EQ(effects.frames, 1);

    // Opaque and normally blended: the colour itself (through the tone curve).
    effects.square(0.0f, 4.0f, {1.0f, 0.0f, 0.0f}, 1.0f);
    CHECK(reddish(centre(offscreen->render(frontCamera(), {}))));
    CHECK_EQ(offscreen->renderer().stats().effectBatches, 1u);
    CHECK_EQ(offscreen->renderer().stats().effectTriangles, 2u);

    // Half see-through white: normal blending lands between the sky and
    // white; adding lands above both in every channel.
    effects.geometry.clear();
    effects.square(0.0f, 4.0f, {1.0f, 1.0f, 1.0f}, 0.5f, 0.0f);
    const Rgba blended = centre(offscreen->render(frontCamera(), {}));
    effects.geometry.clear();
    effects.square(0.0f, 4.0f, {1.0f, 1.0f, 1.0f}, 0.5f, 1.0f);
    const Rgba added = centre(offscreen->render(frontCamera(), {}));
    CHECK(blended.r > sky.r && blended.g > sky.g);
    CHECK(added.r > blended.r && added.g > blended.g && added.b > blended.b);
    // A clear one changes nothing.
    effects.geometry.clear();
    effects.square(0.0f, 4.0f, {1.0f, 1.0f, 1.0f}, 0.0f, 1.0f);
    const Rgba clear = centre(offscreen->render(frontCamera(), {}));
    CHECK(std::abs(clear.r - sky.r) <= 1 && std::abs(clear.g - sky.g) <= 1 && std::abs(clear.b - sky.b) <= 1);
    offscreen->renderer().removeEffects(&effects);
    offscreen->render(frontCamera(), {});
    CHECK_EQ(effects.frames, 5);
}

GPU_TEST(effects_are_hidden_behind_solid_parts)
{
    auto offscreen = makeOffscreen(context);
    const std::vector<DrawItem> items{box({0.0f, 0.0f, 2.0f}, {3.0f, 3.0f, 1.0f}, {0.0f, 0.0f, 1.0f})};
    const Rgba alone = centre(offscreen->render(frontCamera(), items));
    FixedEffects effects;
    offscreen->renderer().addEffects(&effects);
    effects.square(-2.0f, 10.0f, {1.0f, 0.0f, 0.0f}, 1.0f, 1.0f);
    const PixelImage image = offscreen->render(frontCamera(), items);
    CHECK_EQ(centre(image), alone);
    // Beside the part it shows.
    CHECK(image.at(width / 2 + 10, height / 2).r > 150);
}

// AlwaysOnTop: seen through what's in front of it, and over see-through
// parts nearer than it, as Infinite Yield's ESP boxes players through walls.
GPU_TEST(effects_on_top_show_through_everything)
{
    auto offscreen = makeOffscreen(context);
    const std::vector<DrawItem> wall{box({0.0f, 0.0f, 2.0f}, {3.0f, 3.0f, 1.0f}, {0.0f, 0.0f, 1.0f})};
    FixedEffects effects;
    offscreen->renderer().addEffects(&effects);
    effects.square(-2.0f, 10.0f, {1.0f, 0.0f, 0.0f}, 1.0f).onTop = true;
    CHECK(reddish(centre(offscreen->render(frontCamera(), wall))));
    const std::vector<DrawItem> glass{box({0.0f, 0.0f, 2.0f}, {3.0f, 3.0f, 0.2f}, {0.0f, 1.0f, 0.0f}, 0.5f)};
    CHECK(reddish(centre(offscreen->render(frontCamera(), glass))));
}

// The game's BoxHandleAdornment on a part behind a wall: hidden, until
// AlwaysOnTop.
GPU_TEST(effects_adornments_box_a_part_behind_a_wall)
{
    auto offscreen = makeOffscreen(context);
    auto game = lunar::DataModel::create();
    auto part = std::make_shared<lunar::Part>();
    part->setCFrame(CFrame(0.0f, 0.0f, -2.0f));
    part->setParent(game->workspace());
    auto esp = std::make_shared<lunar::BoxHandleAdornment>();
    esp->setAdornee(part.get());
    esp->setSize({2.0f, 2.0f, 2.0f});
    esp->setColor3({1.0f, 0.0f, 0.0f});
    esp->setParent(game->getService<lunar::CoreGui>());
    Adornments adornments(*game, {});
    offscreen->renderer().addEffects(&adornments);
    const std::vector<DrawItem> wall{box({0.0f, 0.0f, 2.0f}, {3.0f, 3.0f, 1.0f}, {0.0f, 0.0f, 1.0f})};
    CHECK(!reddish(centre(offscreen->render(frontCamera(), wall))));
    esp->setAlwaysOnTop(true);
    CHECK(reddish(centre(offscreen->render(frontCamera(), wall))));
    // Beside the wall, red either way.
    esp->setAlwaysOnTop(false);
    esp->setCFrame(CFrame(6.0f, 0.0f, 0.0f));
    CHECK(reddish(offscreen->render(frontCamera(), wall).at(width / 2 + 16, height / 2)));
}

GPU_TEST(effects_sort_with_transparent_parts)
{
    auto offscreen = makeOffscreen(context);
    // Half see-through green in the middle; an opaque red effect behind it
    // shows through, in front of it covers it.
    const std::vector<DrawItem> items{box({0.0f, 0.0f, 0.0f}, {3.0f, 3.0f, 0.2f}, {0.0f, 1.0f, 0.0f}, 0.5f)};
    FixedEffects effects;
    offscreen->renderer().addEffects(&effects);
    effects.square(-3.0f, 4.0f, {1.0f, 0.0f, 0.0f}, 1.0f);
    const Rgba behind = centre(offscreen->render(frontCamera(), items));
    effects.geometry.clear();
    effects.square(3.0f, 2.0f, {1.0f, 0.0f, 0.0f}, 1.0f);
    const Rgba before = centre(offscreen->render(frontCamera(), items));
    CHECK(behind.g >= 60 && behind.r >= 60);
    CHECK(reddish(before));
}

GPU_TEST(effects_light_influence_takes_the_scenes_light)
{
    auto offscreen = makeOffscreen(context);
    FixedEffects effects;
    offscreen->renderer().addEffects(&effects);
    Lighting dark;
    dark.ambient = {0.0f, 0.0f, 0.0f};
    dark.sunColor = {0.0f, 0.0f, 0.0f};
    EffectBatch& batch = effects.square(0.0f, 4.0f, {1.0f, 0.0f, 0.0f}, 1.0f);
    CHECK(reddish(centre(offscreen->render(frontCamera(), {}, dark))));
    batch.lightInfluence = 1.0f;
    const Rgba lit = centre(offscreen->render(frontCamera(), {}, dark));
    CHECK(lit.r <= 10 && lit.g <= 10 && lit.b <= 10);
    // Brightness scales the unlit share.
    batch.lightInfluence = 0.0f;
    batch.brightness = 0.25f;
    const Rgba dim = centre(offscreen->render(frontCamera(), {}, dark));
    CHECK(dim.r > 40 && dim.r < 180);
}

GPU_TEST(effects_take_their_texture)
{
    auto offscreen = makeOffscreen(context);
    FixedEffects effects;
    offscreen->renderer().addEffects(&effects);
    auto image = std::make_shared<lunar::Image>();
    image->width = 2;
    image->height = 2;
    image->rgba.assign(16, 0);
    for (std::size_t i = 0; i < 4; ++i) {
        image->rgba[4 * i + 1] = 255; // green
        image->rgba[4 * i + 3] = 255;
    }
    effects.square(0.0f, 4.0f, {1.0f, 1.0f, 1.0f}, 1.0f).texture = image;
    const Rgba c = centre(offscreen->render(frontCamera(), {}));
    CHECK(c.g >= 180 && c.r <= 40 && c.b <= 40);
}

// A beam and a moving trail through Ribbons, end to end; with
// LUNAR_RIBBON_SHOTS=dir the frames also land there as PNGs to look at.
GPU_TEST(effects_ribbons_draw_beams_and_trails)
{
    auto game = lunar::DataModel::create();
    auto ground = std::make_shared<lunar::Part>();
    ground->setSize({60.0f, 1.0f, 60.0f});
    ground->setCFrame(CFrame(0.0f, -0.5f, 0.0f));
    ground->setColor(Color3::fromRGB(70, 90, 70));
    ground->setAnchored(true);
    ground->setParent(game->workspace());

    // Beams: a straight one; a curved one, FaceCamera, additive, tapering.
    auto post = std::make_shared<lunar::Part>();
    post->setSize({1.0f, 1.0f, 1.0f});
    post->setCFrame(CFrame(0.0f, 4.0f, 0.0f));
    post->setTransparency(1.0f);
    post->setParent(game->workspace());
    auto attach = [&](lunar::Instance& parent, CFrame frame) {
        auto a = std::make_shared<lunar::Attachment>();
        a->setCFrame(frame);
        a->setParent(&parent);
        return a.get();
    };
    // Unturned: the width goes along their Y axes, up, and the camera
    // sees the beam's face.
    auto straight = std::make_shared<lunar::Beam>();
    straight->setAttachment0(attach(*post, CFrame(-10.0f, 2.0f, 0.0f)));
    straight->setAttachment1(attach(*post, CFrame(10.0f, 2.0f, 0.0f)));
    straight->setWidth0(1.0f);
    straight->setWidth1(3.0f);
    straight->setTransparency(lunar::NumberSequence(0.0f));
    straight->setColor(lunar::ColorSequence(
        {{0.0f, Color3(1.0f, 0.2f, 0.1f)}, {0.5f, Color3(1.0f, 1.0f, 0.2f)}, {1.0f, Color3(0.2f, 0.4f, 1.0f)}}));
    straight->setParent(post.get());
    auto curved = std::make_shared<lunar::Beam>();
    // X axes turned up: CurveSize0 lifts the start, a negative CurveSize1
    // the end, into an arch.
    const CFrame up = CFrame::angles(0.0f, 0.0f, std::numbers::pi_v<float> / 2.0f);
    curved->setAttachment0(attach(*post, CFrame(-8.0f, -3.0f, 0.0f) * up));
    curved->setAttachment1(attach(*post, CFrame(8.0f, -3.0f, 0.0f) * up));
    curved->setCurveSize0(8.0f);
    curved->setCurveSize1(-8.0f);
    curved->setFaceCamera(true);
    curved->setLightEmission(1.0f);
    curved->setWidth0(2.0f);
    curved->setWidth1(0.0f);
    curved->setSegments(30);
    curved->setColor(lunar::ColorSequence(Color3(0.3f, 0.8f, 1.0f)));
    curved->setTransparency(lunar::NumberSequence({{0.0f, 0.0f, 0.0f}, {1.0f, 0.8f, 0.0f}}));
    curved->setParent(post.get());

    // A trail on a part that circles round.
    auto mover = std::make_shared<lunar::Part>();
    mover->setSize({1.0f, 1.0f, 1.0f});
    mover->setColor(Color3(1.0f, 1.0f, 1.0f));
    mover->setParent(game->workspace());
    auto trail = std::make_shared<lunar::Trail>();
    trail->setAttachment0(attach(*mover, CFrame(0.0f, -0.75f, 0.0f)));
    trail->setAttachment1(attach(*mover, CFrame(0.0f, 0.75f, 0.0f)));
    trail->setLifetime(1.0f);
    trail->setColor(lunar::ColorSequence({{0.0f, Color3(1.0f, 0.3f, 0.9f)}, {1.0f, Color3(0.2f, 0.2f, 1.0f)}}));
    trail->setTransparency(lunar::NumberSequence({{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}));
    trail->setWidthScale(lunar::NumberSequence({{0.0f, 1.0f, 0.0f}, {1.0f, 0.2f, 0.0f}}));
    trail->setParent(mover.get());

    Ribbons ribbons(*game->workspace(), {});
    const char* shots = std::getenv("LUNAR_RIBBON_SHOTS");
    const std::uint32_t w = shots ? 960 : width;
    const std::uint32_t h = shots ? 540 : height;
    auto offscreen = makeOffscreen(context, w, h);
    offscreen->renderer().addEffects(&ribbons);
    Camera camera;
    camera.cframe = Camera::lookAt({0.0f, 8.0f, 26.0f}, {0.0f, 3.0f, 0.0f});
    Lighting lighting;
    PixelImage image;
    for (int frame = 0; frame <= 45; ++frame) {
        const double time = frame / 60.0;
        const float angle = static_cast<float>(time) * 4.0f;
        mover->setCFrame(CFrame(9.0f * std::cos(angle), 6.0f, 9.0f * std::sin(angle) - 4.0f)
                         * CFrame::angles(0.0f, -angle, 0.0f));
        lighting.time = time;
        image = offscreen->render(camera, extractScene(*game->workspace()), lighting);
    }
    CHECK_EQ(ribbons.beamsDrawn(), 2u);
    CHECK_EQ(ribbons.trailsDrawn(), 1u);
    CHECK_EQ(offscreen->renderer().stats().effectBatches, 3u);
    if (shots) {
        const std::filesystem::path path = std::filesystem::path(shots) / "ribbons.png";
        std::ofstream(path, std::ios::binary) << encodePng(image);
    }
}

// The game's count: 6,209 Beams and 1,039 Trails, spread
// over 1000 studs, the trails on parts that move every frame. Prints what
// Ribbons::collect and whole frames take; LUNAR_RIBBON_BENCH=1 runs it.
GPU_TEST(effects_ribbons_bench)
{
    if (!std::getenv("LUNAR_RIBBON_BENCH"))
        SKIP("set LUNAR_RIBBON_BENCH=1 to run");
    auto game = lunar::DataModel::create();
    std::vector<std::shared_ptr<lunar::Part>> movers;
    std::vector<std::shared_ptr<lunar::Instance>> keep;
    auto attach = [&](lunar::Instance& parent, CFrame frame) {
        auto a = std::make_shared<lunar::Attachment>();
        a->setCFrame(frame);
        a->setParent(&parent);
        return a.get();
    };
    std::uint32_t seed = 1;
    auto random = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    for (int i = 0; i < 6209 + 1039; ++i) {
        auto part = std::make_shared<lunar::Part>();
        part->setSize({1.0f, 1.0f, 1.0f});
        part->setTransparency(1.0f);
        part->setCFrame(CFrame(random() * 1000.0f - 500.0f, random() * 20.0f, random() * 1000.0f - 500.0f));
        part->setParent(game->workspace());
        if (i < 6209) {
            auto beam = std::make_shared<lunar::Beam>();
            beam->setAttachment0(attach(*part, CFrame(-3.0f, 0.0f, 0.0f)));
            beam->setAttachment1(attach(*part, CFrame(3.0f, 0.0f, 0.0f)));
            beam->setCurveSize0(2.0f);
            beam->setFaceCamera(i % 2 == 0);
            beam->setParent(part.get());
        } else {
            auto trail = std::make_shared<lunar::Trail>();
            trail->setAttachment0(attach(*part, CFrame(0.0f, -0.5f, 0.0f)));
            trail->setAttachment1(attach(*part, CFrame(0.0f, 0.5f, 0.0f)));
            trail->setParent(part.get());
            movers.push_back(part);
        }
    }
    Ribbons ribbons(*game->workspace(), {});
    auto offscreen = makeOffscreen(context, 1280, 720);
    offscreen->renderer().addEffects(&ribbons);
    Camera camera;
    camera.cframe = Camera::lookAt({0.0f, 60.0f, 300.0f}, {0.0f, 0.0f, 0.0f});
    const std::vector<DrawItem> items{box({0.0f, -1.0f, 0.0f}, {1000.0f, 1.0f, 1000.0f}, {0.3f, 0.4f, 0.3f})};
    Lighting lighting;
    using Clock = std::chrono::steady_clock;
    double collectMs = 0.0;
    double frameMs = 0.0;
    double recordMs = 0.0;
    constexpr int frames = 120;
    for (int frame = 0; frame < frames + 10; ++frame) {
        lighting.time = frame / 60.0;
        for (std::size_t i = 0; i < movers.size(); ++i)
            movers[i]->setCFrame(movers[i]->cframe() + Vector3{0.2f * std::cos(static_cast<float>(i)), 0.0f, 0.2f});
        // collect on its own, then the frame (which collects again).
        EffectGeometry geometry;
        EffectView view{camera, camera.viewProjection(1280.0f / 720.0f), lighting.time};
        const auto start = Clock::now();
        ribbons.collect(view, geometry);
        const auto collected = Clock::now();
        offscreen->render(camera, items, lighting);
        const auto done = Clock::now();
        if (frame < 10)
            continue; // the trails fill up, the pipelines warm up
        collectMs += std::chrono::duration<double, std::milli>(collected - start).count();
        frameMs += std::chrono::duration<double, std::milli>(done - collected).count();
        recordMs += offscreen->renderer().stats().recordMilliseconds;
    }
    const auto& stats = offscreen->renderer().stats();
    std::println("ribbons bench: {} beams and {} trails, {} and {} drawn, {} batches, {} triangles",
                 ribbons.beamCount(), ribbons.trailCount(), ribbons.beamsDrawn(), ribbons.trailsDrawn(),
                 stats.effectBatches, stats.effectTriangles);
    std::println("ribbons bench: collect {:.2f} ms, frame {:.2f} ms ({:.2f} ms recording, collect included)",
                 collectMs / frames, frameMs / frames, recordMs / frames);
}

GPU_TEST(effects_draw_at_a_render_scale)
{
    auto offscreen = makeOffscreen(context);
    GraphicsSettings graphics;
    graphics.renderScale = 0.5f;
    offscreen->renderer().setGraphics(graphics);
    FixedEffects effects;
    offscreen->renderer().addEffects(&effects);
    effects.square(0.0f, 4.0f, {1.0f, 0.0f, 0.0f}, 1.0f);
    CHECK(reddish(centre(offscreen->render(frontCamera(), {}))));
}
