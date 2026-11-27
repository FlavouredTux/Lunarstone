#include "render_gpu.hpp"

#include <lunar/Classes.hpp>
#include <lunar/render/Graphics.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/Lighting.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/render/Primitives.hpp>

#include <cmath>
#include <cstdlib>
#include <format>
#include <memory>
#include <vector>

// Not all of lunar: the engine's Camera and Lighting would clash with the
// renderer's.
using lunar::CFrame;
using lunar::Color3;
using lunar::DataModel;
using lunar::Vector3;
using namespace lunar::render;

namespace {

constexpr std::uint32_t width = 96;
constexpr std::uint32_t height = 64;

std::unique_ptr<OffscreenRenderer> makeOffscreen(Context& context)
{
    auto offscreen = OffscreenRenderer::create(context, width, height);
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

DrawItem box(Vector3 position, Vector3 size, Color3 color)
{
    return {partMatrix(CFrame(position), size), color, 0.0f};
}

int brightness(Rgba p)
{
    return p.r + p.g + p.b;
}

int difference(Rgba a, Rgba b)
{
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

} // namespace

TEST(post_effects_come_from_lighting_and_the_camera)
{
    auto game = DataModel::create();
    auto* service = game->getService<lunar::Lighting>();
    CHECK(!lightingFrom(*game).post.any());

    // Lighting's children: a bloom, a disabled blur (ignored), sun rays.
    auto bloom = std::make_shared<lunar::BloomEffect>();
    bloom->setIntensity(0.4f);
    bloom->setThreshold(1.5f);
    bloom->setParent(service);
    auto blur = std::make_shared<lunar::BlurEffect>();
    blur->setEnabled(false);
    blur->setParent(service);
    auto rays = std::make_shared<lunar::SunRaysEffect>();
    rays->setParent(service);
    PostLook look = lightingFrom(*game).post;
    CHECK(look.bloom);
    CHECK_EQ(look.bloomIntensity, 0.4f);
    CHECK_EQ(look.bloomThreshold, 1.5f);
    CHECK_EQ(look.blur, 0.0f);
    CHECK(look.sunRays);
    CHECK_EQ(look.sunRaysIntensity, 0.25f);
    CHECK(!look.depthOfField);

    // The camera's count too, and the strongest bloom wins.
    auto camera = std::make_shared<lunar::Camera>();
    camera->setParent(game->workspace());
    game->workspace()->setCurrentCamera(camera.get());
    auto strong = std::make_shared<lunar::BloomEffect>();
    strong->setIntensity(2.0f);
    strong->setParent(camera.get());
    auto cameraBlur = std::make_shared<lunar::BlurEffect>();
    cameraBlur->setSize(10.0f);
    cameraBlur->setParent(camera.get());
    auto dof = std::make_shared<lunar::DepthOfFieldEffect>();
    dof->setFocusDistance(30.0f);
    dof->setParent(camera.get());
    auto tint = std::make_shared<lunar::ColorCorrectionEffect>();
    tint->setBrightness(0.1f);
    tint->setParent(camera.get());
    const Lighting lighting = lightingFrom(*game);
    look = lighting.post;
    CHECK_EQ(look.bloomIntensity, 2.0f);
    CHECK_EQ(look.blur, 10.0f);
    CHECK(look.depthOfField);
    CHECK_EQ(look.focusDistance, 30.0f);
    CHECK(std::abs(lighting.colorCorrection[0][3] - 0.1f) < 1e-5f);
}

TEST(minimal_graphics_leave_post_effects_out)
{
    CHECK(!graphicsSettings(GraphicsPreset::Minimal).postEffects);
    CHECK(graphicsSettings(GraphicsPreset::Average).postEffects);
    CHECK(graphicsSettings(GraphicsPreset::Max).postEffects);
}

GPU_TEST(post_bloom_glows_around_bright_parts)
{
    auto offscreen = makeOffscreen(context);
    // A white part lit hard, in front of a dark wall.
    Lighting lighting;
    lighting.exposure = 4.0f;
    const std::vector<DrawItem> items{box({}, {2.0f, 2.0f, 1.0f}, {1.0f, 1.0f, 1.0f}),
                                      box({0.0f, 0.0f, -20.0f}, {200.0f, 200.0f, 1.0f}, {0.02f, 0.02f, 0.02f})};
    const PixelImage plain = offscreen->render(frontCamera(), items, lighting);
    CHECK(!offscreen->renderer().stats().postEffects);
    lighting.post.bloom = true;
    lighting.post.bloomThreshold = 1.0f;
    const PixelImage glowing = offscreen->render(frontCamera(), items, lighting);
    CHECK(offscreen->renderer().stats().postEffects);
    // Just past the part's edge the wall lights up (sizes scale with the
    // screen, so on this small one the glow is a few pixels); far away it
    // stays as it was.
    std::uint32_t edge = width / 2;
    while (edge < width && brightness(plain.at(edge, height / 2)) > 300)
        ++edge;
    CHECK(edge < width - 8);
    const std::uint32_t x = edge + 1;
    CHECK(brightness(glowing.at(x, height / 2)) > brightness(plain.at(x, height / 2)) + 20);
    CHECK(difference(glowing.at(2, 2), plain.at(2, 2)) <= 3);
    // Under the threshold nothing glows.
    lighting.post.bloomThreshold = 50.0f;
    const PixelImage dim = offscreen->render(frontCamera(), items, lighting);
    CHECK(difference(dim.at(x, height / 2), plain.at(x, height / 2)) <= 2);
}

GPU_TEST(post_effects_leave_colour_correction_applied_once)
{
    // A post effect that changes nothing (bloom no light reaches) leaves the
    // picture as it was, ColorCorrection and all: the post pass applies it
    // instead of the scene, never both.
    auto offscreen = makeOffscreen(context);
    Lighting lighting;
    lighting.colorCorrection[0][3] = 0.2f; // red brightened
    lighting.colorCorrection[2][2] = 0.5f; // blue halved
    const std::vector<DrawItem> items{box({}, {4.0f, 4.0f, 4.0f}, {0.2f, 0.6f, 0.9f})};
    const PixelImage plain = offscreen->render(frontCamera(), items, lighting);
    lighting.post.bloom = true;
    lighting.post.bloomThreshold = 100.0f;
    const PixelImage post = offscreen->render(frontCamera(), items, lighting);
    CHECK(offscreen->renderer().stats().postEffects);
    for (auto [x, y] : {std::pair{width / 2, height / 2}, {2u, 2u}, {width - 3, height / 2}})
        if (difference(post.at(x, y), plain.at(x, y)) > 2)
            lt::fail(std::format("at {},{}: {} {} {} against {} {} {}", x, y, post.at(x, y).r, post.at(x, y).g,
                                 post.at(x, y).b, plain.at(x, y).r, plain.at(x, y).g, plain.at(x, y).b),
                     std::source_location::current());
}

GPU_TEST(post_blur_softens_edges)
{
    auto offscreen = makeOffscreen(context);
    const std::vector<DrawItem> items{box({}, {4.0f, 4.0f, 4.0f}, {1.0f, 0.0f, 0.0f})};
    const PixelImage sharp = offscreen->render(frontCamera(), items);
    Lighting lighting;
    lighting.post.blur = 56.0f; // a few pixels on this small screen
    const PixelImage blurred = offscreen->render(frontCamera(), items, lighting);
    // Find the cube's right edge on the middle row; just past it the red
    // has spread into the sky.
    std::uint32_t edge = width / 2;
    while (edge < width && sharp.at(edge, height / 2).g < 100)
        ++edge;
    CHECK(edge < width - 4);
    const Rgba past = blurred.at(edge, height / 2);
    CHECK(past.g < sharp.at(edge, height / 2).g - 10);
}

GPU_TEST(post_depth_of_field_blurs_what_is_out_of_focus)
{
    auto offscreen = makeOffscreen(context);
    // A red and blue wall far away, meeting in the middle, and a green part
    // in focus below.
    const std::vector<DrawItem> items{box({-50.0f, 0.0f, -90.0f}, {100.0f, 200.0f, 1.0f}, {1.0f, 0.0f, 0.0f}),
                                      box({50.0f, 0.0f, -90.0f}, {100.0f, 200.0f, 1.0f}, {0.0f, 0.0f, 1.0f}),
                                      box({0.0f, -2.5f, 0.0f}, {3.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f})};
    const PixelImage sharp = offscreen->render(frontCamera(), items);
    Lighting lighting;
    lighting.post.depthOfField = true;
    lighting.post.focusDistance = 10.0f;
    lighting.post.inFocusRadius = 3.0f;
    lighting.post.farIntensity = 1.0f;
    const PixelImage focused = offscreen->render(frontCamera(), items, lighting);
    // Beside the seam the far walls mix; the near part keeps its edge.
    const Rgba seam = focused.at(width / 2 - 2, height / 4);
    const Rgba seamBefore = sharp.at(width / 2 - 2, height / 4);
    CHECK(seam.b > seamBefore.b + 20);
    std::uint32_t y = height / 2;
    while (y < height && sharp.at(width / 2, y).g < 100)
        ++y;
    CHECK(y < height - 2);
    CHECK(difference(focused.at(width / 2, y + 1), sharp.at(width / 2, y + 1)) <= 6);
}

GPU_TEST(post_sun_rays_brighten_towards_the_sun)
{
    auto offscreen = makeOffscreen(context);
    Lighting lighting;
    Camera camera;
    camera.cframe = Camera::lookAt({}, lighting.sunDirection * 10.0f);
    const PixelImage plain = offscreen->render(camera, {}, lighting);
    lighting.post.sunRays = true;
    const PixelImage rays = offscreen->render(camera, {}, lighting);
    const Rgba nearSun = rays.at(width / 2 + 12, height / 2);
    CHECK(brightness(nearSun) > brightness(plain.at(width / 2 + 12, height / 2)) + 15);
    // Facing away from the sun there are none.
    camera.cframe = Camera::lookAt({}, lighting.sunDirection * -10.0f);
    const PixelImage away = offscreen->render(camera, {}, lighting);
    lighting.post.sunRays = false;
    const PixelImage awayPlain = offscreen->render(camera, {}, lighting);
    CHECK(difference(away.at(width / 2, height / 2), awayPlain.at(width / 2, height / 2)) <= 2);
}

GPU_TEST(post_effects_scale_a_smaller_scene_up)
{
    // At half size the post pass draws the scene onto the whole target.
    auto offscreen = makeOffscreen(context);
    GraphicsSettings graphics;
    graphics.renderScale = 0.5f;
    offscreen->renderer().setGraphics(graphics);
    const std::vector<DrawItem> items{box({}, {4.0f, 4.0f, 4.0f}, {1.0f, 0.0f, 0.0f})};
    const PixelImage plain = offscreen->render(frontCamera(), items);
    Lighting lighting;
    lighting.post.bloom = true;
    lighting.post.bloomThreshold = 100.0f;
    const PixelImage post = offscreen->render(frontCamera(), items, lighting);
    CHECK(offscreen->renderer().stats().postEffects);
    for (auto [x, y] : {std::pair{width / 2, height / 2}, {0u, 0u}, {width - 1, height - 1}})
        CHECK(difference(post.at(x, y), plain.at(x, y)) <= 3);
}

TEST(post_traced_sun_rays_light_the_air_the_sun_reaches)
{
    // A roof with a slit, the sun straight above: under the slit the air is
    // lit, so looking along it shows a shaft; looking off to the side, the
    // air's in the roof's shadow. The sun isn't on screen at all.
    Context* rays = lt::rayTracingGpu();
    if (!rays)
        SKIP("no ray queries");
    Context& context = *rays;
    context.takeValidationErrors();
    auto offscreen = makeOffscreen(context);
    const Color3 dark{0.05f, 0.05f, 0.05f};
    const std::vector<DrawItem> items{
        box({-100.75f, 8.0f, -55.0f}, {198.5f, 1.0f, 130.0f}, dark),
        box({100.75f, 8.0f, -55.0f}, {198.5f, 1.0f, 130.0f}, dark),
        box({0.0f, -1.0f, -55.0f}, {400.0f, 1.0f, 130.0f}, dark), box({0.0f, 3.0f, -112.0f}, {400.0f, 30.0f, 1.0f}, dark)};
    Camera camera;
    camera.cframe = Camera::lookAt({0.0f, 2.0f, 5.0f}, {0.0f, 2.0f, -100.0f});
    Lighting lighting;
    lighting.sunDirection = {0.0f, 1.0f, 0.0f};
    const PixelImage plain = offscreen->render(camera, items, lighting);
    lighting.post.sunRays = true;
    lighting.post.sunRaysIntensity = 1.0f;
    const PixelImage traced = offscreen->render(camera, items, lighting);
    auto gain = [&](std::uint32_t x) {
        return brightness(traced.at(x, height / 2)) - brightness(plain.at(x, height / 2));
    };
    CHECK(gain(width / 2) > gain(width / 4) + 20);
    CHECK(gain(width / 4) < 15);
    lt::checkNoValidationErrors(context);
}
